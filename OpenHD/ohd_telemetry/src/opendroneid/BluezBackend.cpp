// SPDX-License-Identifier: GPL-3.0-or-later
#include "BluezBackend.h"

#include <gio/gio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <tuple>

namespace openhd::odid {
namespace {
using Clock = std::chrono::steady_clock;
constexpr char kAdvertisement[] = "org.bluez.LEAdvertisement1";
constexpr char kManager[] = "org.bluez.LEAdvertisingManager1";
constexpr char kPath[] = "/org/openhd/odid/advertisement";
constexpr char kUuid[] = "0000fffa-0000-1000-8000-00805f9b34fb";
constexpr char kXml[] =
    "<node><interface name='org.bluez.LEAdvertisement1'>"
    "<method name='Release'/><property name='Type' type='s' access='read'/>"
    "<property name='ServiceData' type='a{sv}' access='read'/>"
    "</interface></node>";

// Test overrides exist ONLY in the isolated test translation unit. Production
// ignores DBUS_SYSTEM_BUS_ADDRESS and has no adapter/validation bypass switch.
#ifdef OPENHD_ODID_BLUEZ_ISOLATED_TEST
std::string test_bus_address;
std::string test_sys_root;
#endif
std::string bus_address() {
#ifdef OPENHD_ODID_BLUEZ_ISOLATED_TEST
  return test_bus_address;
#else
  return "unix:path=/run/dbus/system_bus_socket";
#endif
}
std::string sys_root() {
#ifdef OPENHD_ODID_BLUEZ_ISOLATED_TEST
  return test_sys_root;
#else
  return "/sys";
#endif
}
int remaining(Clock::time_point deadline) {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - Clock::now()).count();
  return static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(500, ms)));
}
std::string reason(const char* operation, GError* error) {
  // Remote messages can contain arbitrary/large/private text: retain only the
  // useful D-Bus error identifier, or the local GLib error code.
  gchar* remote = error ? g_dbus_error_get_remote_error(error) : nullptr;
  char text[192];
  std::snprintf(text, sizeof(text), "%s: %.120s (%d)", operation,
                remote ? remote : (error ? g_quark_to_string(error->domain)
                                          : "deadline/registration lost"),
                error ? error->code : 0);
  g_free(remote);
  return text;
}
bool hci_valid(const std::string& name) {
  return name.size() >= 4 && name.size() <= 8 &&
         name.compare(0, 3, "hci") == 0 &&
         (name.size() == 4 || name[3] != '0') &&
         std::all_of(name.begin() + 3, name.end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}
std::string read_small(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  char data[4096];
  file.read(data, sizeof(data));
  return std::string(data, static_cast<size_t>(file.gcount()));
}
bool controller_allowed(const std::string& name) {
  // Fail closed on missing evidence. No USB adapter, no inferred hci0, and no
  // UART attach/overlay changes. Works with DT serdev hci_uart/bcm controllers;
  // older hciattach layouts without this evidence are intentionally rejected.
  namespace fs = std::filesystem;
  const fs::path root(sys_root());
  const auto board = read_small(root / "firmware/devicetree/base/compatible");
  bool raspberry = false;
  for (size_t p = 0; p < board.size();) {
    const auto end = board.find('\0', p);
    constexpr char prefix[] = "raspberrypi,";
    if (board.compare(p, sizeof(prefix) - 1, prefix) == 0) raspberry = true;
    if (end == std::string::npos) break;
    p = end + 1;
  }
  if (!raspberry) return false;
  auto device = fs::canonical(root / "class/bluetooth" / name / "device");
  const auto devices = fs::canonical(root / "devices");
  const auto relative = device.lexically_relative(devices);
  if (relative.empty() || *relative.begin() == "..") return false;
  bool uart = false, broadcom = false;
  for (int i = 0; i < 10 && device != devices && device != device.root_path();
       ++i, device = device.parent_path()) {
    const auto component = device.filename().string();
    if (component.find("usb") != std::string::npos) return false;
    const auto compatible = read_small(device / "of_node/compatible");
    // Broadcom Bluetooth DT binding, not merely a Broadcom SoC ancestor.
    for (size_t p = 0; p < compatible.size();) {
      auto end = compatible.find('\0', p);
      auto token = compatible.substr(p, end == std::string::npos ? end : end - p);
      if (token.rfind("brcm,bcm", 0) == 0 &&
          token.size() >= 14 && token.compare(token.size() - 3, 3, "-bt") == 0)
        broadcom = true;
      if (end == std::string::npos) break;
      p = end + 1;
    }
    std::error_code ec;
    const auto subsystem = fs::canonical(device / "subsystem", ec).filename();
    if (!ec && (subsystem == "serial" || subsystem == "tty")) uart = true;
  }
  return uart && broadcom;
}
}  // namespace

struct BluezBackend::State : std::enable_shared_from_this<State> {
  GMainContext* context = g_main_context_new();
  GMainLoop* loop = g_main_loop_new(context, FALSE);
  GCancellable* cancel = g_cancellable_new();
  GDBusConnection* connection = nullptr;
  guint object = 0, watch = 0;
  std::promise<void> ready;
  // 0 inactive/released, 1 registration pending, 2 registered. CAS prevents a
  // Release/owner loss during RegisterAdvertisement being overwritten by success.
  std::atomic<int> registration{0};
  std::atomic<bool> owner_lost{false};
  bool connecting = true, stopping = false, possible = false;
  std::array<uint8_t, 27> payload{};  // immutable while exported/registered
  std::string adapter, owner, address;
  std::string failure;

  ~State() {
    g_object_unref(cancel);
    g_main_loop_unref(loop);
    g_main_context_unref(context);
  }
  // Only the context thread tears down exports. It owns State until all
  // asynchronous connection callbacks have completed, even after API timeout.
  void cleanup() noexcept {
    stopping = true;
    registration.store(0);
    if (connecting) return;
    if (connection) {
      if (watch) g_dbus_connection_signal_unsubscribe(connection, watch);
      if (object) g_dbus_connection_unregister_object(connection, object);
      // Private connection: disconnect also removes only this client's adverts,
      // including a RegisterAdvertisement that completed after our timeout.
      auto* closing = connection;
      connection = nullptr;
      g_dbus_connection_close(closing, nullptr,
          [](GObject* object, GAsyncResult* result, gpointer data) {
            auto& s = *static_cast<State*>(data);
            GError* error = nullptr;
            g_dbus_connection_close_finish(G_DBUS_CONNECTION(object), result, &error);
            g_clear_error(&error);
            g_object_unref(object);
            // Dispatch already queued signal/subscription destroy notifications
            // before stopping the context that owns their callbacks.
            auto* source = g_idle_source_new();
            g_source_set_priority(source, G_PRIORITY_LOW);
            g_source_set_callback(source, [](gpointer p) -> gboolean {
              g_main_loop_quit(static_cast<State*>(p)->loop);
              return G_SOURCE_REMOVE;
            }, &s, nullptr);
            g_source_attach(source, s.context);
            g_source_unref(source);
          }, this);
      return;
    }
    g_main_loop_quit(loop);
  }
  static void method(GDBusConnection*, const gchar* sender, const gchar*,
                     const gchar*, const gchar* method, GVariant*,
                     GDBusMethodInvocation* invocation, gpointer data) noexcept {
    auto& s = *static_cast<State*>(data);
    if (s.owner != sender) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "org.freedesktop.DBus.Error.AccessDenied", "Not owner");
    } else if (std::strcmp(method, "Release") == 0) {
      s.registration.store(0);
      g_dbus_method_invocation_return_value(invocation, nullptr);
    } else {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown");
    }
  }
  static GVariant* property(GDBusConnection*, const gchar* sender, const gchar*,
                            const gchar*, const gchar* property, GError** error,
                            gpointer data) noexcept {
    auto& s = *static_cast<State*>(data);
    if (s.owner != sender) {
      g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
                          "Not owner");
      return nullptr;
    }
    if (std::strcmp(property, "Type") == 0) return g_variant_new_string("broadcast");
    GVariantBuilder dictionary;
    g_variant_builder_init(&dictionary, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&dictionary, "{sv}", kUuid,
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, s.payload.data(),
                                  s.payload.size(), sizeof(uint8_t)));
    return g_variant_builder_end(&dictionary);
  }
  static void changed(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                      const gchar*, GVariant* parameters, gpointer data) noexcept {
    auto& s = *static_cast<State*>(data);
    const gchar *name, *old_owner, *new_owner;
    g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
    if (s.owner != new_owner) {
      s.owner_lost.store(true);
      s.registration.store(0);
    }
  }
  void begin() {
    // Called with this context thread-default. No synchronous bus authentication.
    auto* hold = new std::shared_ptr<State>(shared_from_this());
    g_dbus_connection_new_for_address(address.c_str(),
        static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, cancel,
        [](GObject*, GAsyncResult* result, gpointer data) noexcept {
          std::unique_ptr<std::shared_ptr<State>> hold(
              static_cast<std::shared_ptr<State>*>(data));
          auto& s = **hold;
          GError* error = nullptr;
          s.connection = g_dbus_connection_new_for_address_finish(result, &error);
          s.connecting = false;
          try {
            if (error) s.failure = reason("system bus", error);
            if (s.connection) g_dbus_connection_set_exit_on_close(s.connection, FALSE);
          } catch (...) { /* API will see no usable registration. */ }
          g_clear_error(&error);
          s.ready.set_value();
          if (s.stopping) s.cleanup();
        }, hold);
  }
  GVariant* call(const char* destination, const char* path, const char* interface,
                 const char* method, GVariant* parameters, const GVariantType* type,
                 Clock::time_point deadline, GError** error) {
    const int ms = remaining(deadline);
    if (!ms) {
      if (parameters) g_variant_unref(g_variant_ref_sink(parameters));
      return nullptr;
    }
    return g_dbus_connection_call_sync(connection, destination, path, interface,
        method, parameters, type, G_DBUS_CALL_FLAGS_NO_AUTO_START, ms, nullptr, error);
  }
};

BluezBackend::~BluezBackend() { stop(); }
bool BluezBackend::active() const {
  // As with start/stop, serialize access to state_ on the owning caller worker.
  return state_ && state_->registration.load() == 2 &&
         !state_->owner_lost.load() &&
         !g_dbus_connection_is_closed(state_->connection);
}
void BluezBackend::stop() noexcept {
  auto s = std::move(state_);
  if (!s) return;
  const bool registered = s->registration.exchange(0) != 0;
  try {
    if (registered && s->possible && s->connection && !g_dbus_connection_is_closed(s->connection)) {
      GError* error = nullptr;
      auto* reply = s->call(s->owner.c_str(), s->adapter.c_str(), kManager,
          "UnregisterAdvertisement", g_variant_new("(o)", kPath),
          G_VARIANT_TYPE_UNIT, Clock::now() + std::chrono::milliseconds(500), &error);
      if (reply) g_variant_unref(reply);
      g_clear_error(&error);  // stop is best effort; never unregister another path.
    }
  } catch (...) { /* Still cancel and close our private connection below. */ }
  g_cancellable_cancel(s->cancel);
  // The loop thread itself retains State until cleanup/async completion. This
  // enqueue needs no C++ allocation, including during exception recovery.
  g_main_context_invoke_full(s->context, G_PRIORITY_DEFAULT,
      [](gpointer p) -> gboolean {
        static_cast<State*>(p)->cleanup();
        return G_SOURCE_REMOVE;
      }, s.get(), nullptr);
}

std::string BluezBackend::start_or_update(const std::array<uint8_t, 25>& frame,
                                         uint8_t counter, const std::string& hci_name) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(900);
  // Error recovery closes the private connection rather than adding a second
  // blocking unregister budget to an already expired start/update operation.
  auto fail = [this](std::string text) {
    if (state_) state_->possible = false;
    stop();
    return text;
  };
  try {
    if (!hci_valid(hci_name)) return fail("explicit canonical hciN required");
    if (!controller_allowed(hci_name))
      return fail("controller rejected: Raspberry Pi UART Broadcom DT evidence required");
    if (state_ && (state_->adapter != "/org/bluez/" + hci_name ||
                   state_->owner_lost.load() ||
                   g_dbus_connection_is_closed(state_->connection)))
      return fail("adapter/BlueZ changed; stopped, explicit retry required");
    if (!state_) {
      auto s = std::make_shared<State>();
      s->adapter = "/org/bluez/" + hci_name;
      s->address = bus_address();
      auto ready = s->ready.get_future();
      // A detached loop retains its state until cleanup and outstanding async
      // connection completion. No API timeout can leave a dangling callback.
      std::thread thread([s] {
        g_main_context_push_thread_default(s->context);
        try { s->begin(); } catch (...) {
          s->connecting = false;
          s->ready.set_value();
        }
        g_main_loop_run(s->loop);
        g_main_context_pop_thread_default(s->context);
      });
      thread.detach();
      state_ = s;
      if (ready.wait_until(std::min(deadline, Clock::now() +
                                   std::chrono::milliseconds(500))) != std::future_status::ready)
        return fail("system bus: connect/auth timeout");
      if (!s->connection) return fail(s->failure.empty() ? "system bus unavailable" : s->failure);
      GError* error = nullptr;
      auto* reply = s->call("org.freedesktop.DBus", "/org/freedesktop/DBus",
          "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", "org.bluez"),
          G_VARIANT_TYPE("(s)"), deadline, &error);
      if (!reply) { auto why = reason("BlueZ owner", error); g_clear_error(&error); return fail(why); }
      const gchar* owner = nullptr;
      g_variant_get(reply, "(&s)", &owner);
      s->owner = owner;
      g_variant_unref(reply);
    }
    auto s = state_;
    std::array<uint8_t, 27> payload{};
    payload[0] = 0x0d; payload[1] = counter;
    std::copy(frame.begin(), frame.end(), payload.begin() + 2);
    if (active() && payload == s->payload) return {};
    if (s->registration.exchange(0) == 0) s->possible = false;
    GError* error = nullptr;
    if (s->possible) {
      auto* reply = s->call(s->owner.c_str(), s->adapter.c_str(), kManager,
          "UnregisterAdvertisement", g_variant_new("(o)", kPath),
          G_VARIANT_TYPE_UNIT, deadline, &error);
      if (!reply) { auto why = reason("unregister", error); g_clear_error(&error); return fail(why); }
      g_variant_unref(reply);
      s->possible = false;
    }
    // Update payload/export on the callback thread, never concurrently with
    // GetAll. Reuse the same object path and same selected manager each rotation.
    auto completed = std::make_shared<std::promise<bool>>();
    auto exported = completed->get_future();
    using Export = std::tuple<std::shared_ptr<State>, std::array<uint8_t, 27>,
                              std::shared_ptr<std::promise<bool>>>;
    auto* task = new Export(s, payload, completed);
    g_main_context_invoke_full(s->context, G_PRIORITY_DEFAULT,
        [](gpointer p) -> gboolean {
          auto& [s, bytes, done] = *static_cast<Export*>(p);
          if (s->stopping || !s->connection) {
            done->set_value(false);
            return G_SOURCE_REMOVE;
          }
          s->payload = bytes;
          GError* error = nullptr;
          if (!s->object) {
            auto* xml = g_dbus_node_info_new_for_xml(kXml, &error);
            if (xml) {
              static const GDBusInterfaceVTable table = {State::method, State::property, nullptr, {nullptr}};
              s->object = g_dbus_connection_register_object(s->connection, kPath,
                  xml->interfaces[0], &table, s.get(), nullptr, &error);
              g_dbus_node_info_unref(xml);
            }
            s->watch = g_dbus_connection_signal_subscribe(s->connection,
                "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                "/org/freedesktop/DBus", "org.bluez", G_DBUS_SIGNAL_FLAGS_NONE,
                State::changed, s.get(), nullptr);
          }
          g_clear_error(&error);
          done->set_value(s->object != 0);
          return G_SOURCE_REMOVE;
        }, task, [](gpointer p) { delete static_cast<Export*>(p); });
    if (exported.wait_until(deadline) != std::future_status::ready || !exported.get())
      return fail("advertisement export failed/timeout");
    s->registration.store(1);
    s->possible = true;
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    auto* reply = s->call(s->owner.c_str(), s->adapter.c_str(), kManager,
        "RegisterAdvertisement", g_variant_new("(oa{sv})", kPath, &options),
        G_VARIANT_TYPE_UNIT, deadline, &error);
    if (!reply) { auto why = reason("register", error); g_clear_error(&error); return fail(why); }
    g_variant_unref(reply);
    // Close the owner-subscription race; never count a former owner as current.
    reply = s->call("org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", "org.bluez"),
        G_VARIANT_TYPE("(s)"), deadline, &error);
    if (!reply) { auto why = reason("verify BlueZ owner", error); g_clear_error(&error); return fail(why); }
    const gchar* current = nullptr;
    g_variant_get(reply, "(&s)", &current);
    const bool same = s->owner == current;
    g_variant_unref(reply);
    int pending = 1;
    if (!same || s->owner_lost.load() || !s->registration.compare_exchange_strong(pending, 2))
      return fail("advertisement released/BlueZ owner lost during registration");
    return {};
  } catch (...) {
    return fail("BlueZ backend exception/sysfs unavailable; stopped");
  }
}
}  // namespace openhd::odid