// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone translation unit: private dbus-daemon + synthetic sysfs ONLY.
// Never compile this file into OpenHD or link it with BluezBackend.cpp again.
#define OPENHD_ODID_BLUEZ_ISOLATED_TEST
#include "BluezBackend.cpp"
#include <gio/gunixsocketaddress.h>

#include <cstdlib>
#include <iostream>
#include <mutex>
#include <set>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
using namespace openhd::odid;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {
GDBusConnection* connect_test_bus() {
  GError* error = nullptr;
  auto* c = g_dbus_connection_new_for_address_sync(test_bus_address.c_str(),
      static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, &error);
  CHECK(c && !error);
  g_dbus_connection_set_exit_on_close(c, FALSE);
  return c;
}
void write(const fs::path& path, const std::string& value) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << value;
  CHECK(out.good());
}
void sysfs_fixture(const fs::path& root) {
  fs::create_directories(root / "class/bluetooth");
  fs::create_directories(root / "bus/serial");
  const auto serial = root / "devices/platform/serial/serial0/serial0-0";
  fs::create_directories(serial);
  fs::create_directory_symlink(root / "bus/serial", serial / "subsystem");
  write(serial / "of_node/compatible", std::string("brcm,bcm43438-bt\0", 17));
  fs::create_directories(root / "class/bluetooth/hci7");
  fs::create_directory_symlink(serial, root / "class/bluetooth/hci7/device");
  write(root / "firmware/devicetree/base/compatible",
        std::string("raspberrypi,model-zero-2-w\0brcm,bcm2837\0", 37));
}

class MockBluez {
 public:
  enum Mode { Normal, Busy, InvalidLength, NoReply, ReleaseDuringRegister,
              UnregisterNoReply };
  std::atomic<int> mode{Normal}, registrations{0}, unregistrations{0};
  GMainContext* context = g_main_context_new();
  GMainLoop* loop = g_main_loop_new(context, FALSE);
  GDBusConnection* connection = nullptr;
  std::thread thread;
  std::mutex mutex;
  std::string client, path;
  std::set<std::string> clients;
  std::vector<std::array<uint8_t, 27>> frames;
  std::vector<GDBusMethodInvocation*> held;
  guint object = 0;

  MockBluez() {
    std::promise<void> ready;
    auto done = ready.get_future();
    thread = std::thread([this, &ready] {
      g_main_context_push_thread_default(context);
      connection = connect_test_bus();
      GError* error = nullptr;
      auto* reply = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus",
          "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
          g_variant_new("(su)", "org.bluez", 4u), G_VARIANT_TYPE("(u)"),
          G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr, &error);
      CHECK(reply && !error);
      g_variant_unref(reply);
      auto* xml = g_dbus_node_info_new_for_xml(
          "<node><interface name='org.bluez.LEAdvertisingManager1'>"
          "<method name='RegisterAdvertisement'><arg type='o' direction='in'/>"
          "<arg type='a{sv}' direction='in'/></method>"
          "<method name='UnregisterAdvertisement'><arg type='o' direction='in'/>"
          "</method></interface></node>", &error);
      CHECK(xml && !error);
      static const GDBusInterfaceVTable table = {dispatch, nullptr, nullptr, {nullptr}};
      object = g_dbus_connection_register_object(connection, "/org/bluez/hci7",
          xml->interfaces[0], &table, this, nullptr, &error);
      CHECK(object && !error);
      g_dbus_node_info_unref(xml);
      ready.set_value();
      g_main_loop_run(loop);
      for (auto* invocation : held) {
        g_dbus_method_invocation_return_dbus_error(invocation,
            "org.bluez.Error.Failed", "test cleanup");
        g_object_unref(invocation);
      }
      g_dbus_connection_unregister_object(connection, object);
      g_dbus_connection_close_sync(connection, nullptr, nullptr);
      g_object_unref(connection);
      g_main_context_pop_thread_default(context);
    });
    done.get();
  }
  ~MockBluez() {
    g_main_loop_quit(loop);
    thread.join();
    g_main_loop_unref(loop);
    g_main_context_unref(context);
  }
  void release(GDBusConnection* sender = nullptr) {
    std::string destination, object_path;
    { std::lock_guard<std::mutex> lock(mutex); destination = client; object_path = path; }
    GError* error = nullptr;
    auto* reply = g_dbus_connection_call_sync(sender ? sender : connection,
        destination.c_str(), object_path.c_str(), kAdvertisement, "Release",
        nullptr, G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr, &error);
    if (sender) CHECK(!reply && error && g_dbus_error_is_remote_error(error));
    else CHECK(reply && !error);
    if (reply) g_variant_unref(reply);
    g_clear_error(&error);
  }
  void drop_name() {
    auto* reply = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName",
        g_variant_new("(s)", "org.bluez"), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr, nullptr);
    CHECK(reply);
    g_variant_unref(reply);
  }
  bool last_client_gone() {
    std::string peer;
    { std::lock_guard<std::mutex> lock(mutex); peer = client; }
    GError* error = nullptr;
    auto* reply = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
        g_variant_new("(s)", peer.c_str()), G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr, &error);
    const bool gone = reply == nullptr && error != nullptr;
    if (reply) g_variant_unref(reply);
    g_clear_error(&error);
    return gone;
  }
 private:
  static void dispatch(GDBusConnection* c, const gchar* sender, const gchar*,
                       const gchar*, const gchar* method, GVariant* args,
                       GDBusMethodInvocation* invocation, gpointer data) {
    auto& self = *static_cast<MockBluez*>(data);
    const gchar* object_path = nullptr;
    if (std::strcmp(method, "UnregisterAdvertisement") == 0) {
      g_variant_get(args, "(&o)", &object_path);
      CHECK(std::strcmp(object_path, kPath) == 0);
      { std::lock_guard<std::mutex> lock(self.mutex); CHECK(self.clients.count(sender) == 1); }
      ++self.unregistrations;
      if (self.mode == UnregisterNoReply) {
        self.held.push_back(G_DBUS_METHOD_INVOCATION(g_object_ref(invocation)));
        return;
      }
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    CHECK(std::strcmp(method, "RegisterAdvertisement") == 0);
    GVariant* options = nullptr;
    g_variant_get(args, "(&o@a{sv})", &object_path, &options);
    CHECK(g_variant_n_children(options) == 0);
    g_variant_unref(options);
    CHECK(std::strcmp(object_path, kPath) == 0);
    {
      std::lock_guard<std::mutex> lock(self.mutex);
      if (!self.path.empty()) CHECK(self.path == object_path);
      self.client = sender; self.path = object_path;
      self.clients.insert(sender);
    }
    ++self.registrations;
    const int mode = self.mode.load();
    if (mode == Busy || mode == InvalidLength) {
      g_dbus_method_invocation_return_dbus_error(invocation,
          mode == Busy ? "org.bluez.Error.NotPermitted" : "org.bluez.Error.InvalidLength",
          "synthetic private data must not be echoed");
      return;
    }
    if (mode == NoReply) {
      self.held.push_back(G_DBUS_METHOD_INVOCATION(g_object_ref(invocation)));
      return;
    }
    struct Read { MockBluez* self; GDBusMethodInvocation* invocation; int mode; };
    auto* read = new Read{&self, G_DBUS_METHOD_INVOCATION(g_object_ref(invocation)), mode};
    // Do NOT reply to Register until GetAll completes: this is the deadlock
    // regression test against a caller blocking the export's main context.
    g_dbus_connection_call(c, sender, object_path, "org.freedesktop.DBus.Properties",
        "GetAll", g_variant_new("(s)", kAdvertisement), G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr,
        [](GObject* object, GAsyncResult* result, gpointer data) {
          std::unique_ptr<Read> read(static_cast<Read*>(data));
          GError* error = nullptr;
          auto* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
          CHECK(reply && !error);
          GVariant* properties = nullptr;
          g_variant_get(reply, "(@a{sv})", &properties);
          CHECK(g_variant_n_children(properties) == 2);
          const gchar* type = nullptr;
          CHECK(g_variant_lookup(properties, "Type", "&s", &type));
          CHECK(std::strcmp(type, "broadcast") == 0);
          auto* service = g_variant_lookup_value(properties, "ServiceData", G_VARIANT_TYPE("a{sv}"));
          CHECK(service && g_variant_n_children(service) == 1);
          auto* bytes = g_variant_lookup_value(service, kUuid, G_VARIANT_TYPE("ay"));
          CHECK(bytes);
          gsize length = 0;
          const auto* raw = static_cast<const uint8_t*>(g_variant_get_fixed_array(bytes, &length, 1));
          CHECK(length == 27 && raw[0] == 0x0d);
          std::array<uint8_t, 27> frame{};
          std::copy(raw, raw + length, frame.begin());
          { std::lock_guard<std::mutex> lock(read->self->mutex); read->self->frames.push_back(frame); }
          g_variant_unref(bytes); g_variant_unref(service);
          g_variant_unref(properties); g_variant_unref(reply);
          if (read->mode == ReleaseDuringRegister) read->self->release();
          g_dbus_method_invocation_return_value(read->invocation, nullptr);
          g_object_unref(read->invocation);
        }, read);
  }
};
template <typename F> void eventually(F predicate) {
  const auto deadline = Clock::now() + 1s;
  while (!predicate() && Clock::now() < deadline) std::this_thread::yield();
  CHECK(predicate());
}
template <typename F> std::string bounded(F action) {
  const auto before = Clock::now();
  const auto result = action();
  CHECK(Clock::now() - before < 1s);
  CHECK(result.size() <= 191);
  return result;
}
}  // namespace

int main() {
  gchar* temp = g_dir_make_tmp("odid-bluez-test-XXXXXX", nullptr);
  CHECK(temp);
  const fs::path root(temp);
  g_free(temp);
  test_sys_root = root.string();
  test_bus_address = "unix:path=/nonexistent-odid-test-bus";
  std::array<uint8_t, 25> frame{};
  for (size_t i = 0; i < frame.size(); ++i) frame[i] = static_cast<uint8_t>(i + 1);
  {
    BluezBackend disabled;
    CHECK(!disabled.active()); disabled.stop(); disabled.stop();
    for (const auto& invalid : {"", "hci", "hci-1", "hci00", "hci7/../0", "hci7;reboot", "hci123456"})
      CHECK(bounded([&] { return disabled.start_or_update(frame, 0, invalid); }).find("hciN") != std::string::npos);
    CHECK(!disabled.start_or_update(frame, 0, "hci7").empty());
    sysfs_fixture(root);
    CHECK(controller_allowed("hci7"));
    const auto serial = root / "devices/platform/serial/serial0/serial0-0";
    fs::remove(serial / "subsystem");
    CHECK(!controller_allowed("hci7"));
    fs::create_directory_symlink(root / "bus/serial", serial / "subsystem");
    write(serial / "of_node/compatible", "brcm,bcm2837");
    CHECK(!controller_allowed("hci7"));
    write(serial / "of_node/compatible", "brcm,bcm43438-bt");
    const auto usb = root / "devices/platform/usb1/serial0-0";
    fs::create_directories(usb);
    fs::create_directory_symlink(root / "bus/serial", usb / "subsystem");
    write(usb / "of_node/compatible", "brcm,bcm43438-bt");
    fs::remove(root / "class/bluetooth/hci7/device");
    fs::create_directory_symlink(usb, root / "class/bluetooth/hci7/device");
    CHECK(!controller_allowed("hci7"));
    fs::remove(root / "class/bluetooth/hci7/device");
    fs::create_directory_symlink(serial, root / "class/bluetooth/hci7/device");
    write(root / "firmware/devicetree/base/compatible", "brcm,bcm2837");
    CHECK(!controller_allowed("hci7"));
    write(root / "firmware/devicetree/base/compatible", "raspberrypi,model-zero-2-w");
    CHECK(!bounded([&] { return disabled.start_or_update(frame, 0, "hci7"); }).empty());
  }
  {
    // A socket that accepts TCP-style connection setup but never speaks D-Bus
    // authentication: verify that initial connection is bounded too.
    auto* listener = g_socket_listener_new();
    const auto socket = (root / "silent-bus").string();
    auto* address = g_unix_socket_address_new(socket.c_str());
    CHECK(g_socket_listener_add_address(listener, address, G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_DEFAULT, nullptr, nullptr, nullptr));
    g_object_unref(address);
    test_bus_address = "unix:path=" + socket;
    BluezBackend backend;
    CHECK(bounded([&] { return backend.start_or_update(frame, 0, "hci7"); }).find("timeout") != std::string::npos);
    CHECK(!backend.active());
    g_socket_listener_close(listener);
    g_object_unref(listener);
  }
  auto* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);  // Private synthetic bus; never the system bus.
  test_bus_address = g_test_dbus_get_bus_address(bus);
  {
    BluezBackend absent;
    const auto why = bounded([&] { return absent.start_or_update(frame, 0, "hci7"); });
    CHECK(why.find("NameHasNoOwner") != std::string::npos && !absent.active());
  }
  {
    MockBluez bluez;
    BluezBackend backend;
    CHECK(bounded([&] { return backend.start_or_update(frame, 254, "hci7"); }).empty());
    CHECK(backend.active() && bluez.registrations == 1);
    CHECK(backend.start_or_update(frame, 254, "hci7").empty());
    CHECK(bluez.registrations == 1 && bluez.unregistrations == 0);
    frame[24] = 0xa5;
    CHECK(backend.start_or_update(frame, 255, "hci7").empty());
    CHECK(bluez.registrations == 2 && bluez.unregistrations == 1);
    {
      std::lock_guard<std::mutex> lock(bluez.mutex);
      CHECK(bluez.frames.size() == 2 && bluez.frames[0][1] == 254 && bluez.frames[1][1] == 255);
      CHECK(std::equal(frame.begin(), frame.end(), bluez.frames[1].begin() + 2));
    }
    auto* intruder = connect_test_bus();
    bluez.release(intruder);
    CHECK(backend.active());
    g_dbus_connection_close_sync(intruder, nullptr, nullptr); g_object_unref(intruder);
    bluez.release();
    CHECK(!backend.active());
    CHECK(backend.start_or_update(frame, 0, "hci7").empty());
    CHECK(bluez.unregistrations == 1);  // Release needs no Unregister.
    backend.stop(); CHECK(!backend.active());
    CHECK(bluez.unregistrations == 2);
    backend.stop(); CHECK(bluez.unregistrations == 2);
    for (int mode : {MockBluez::Busy, MockBluez::InvalidLength, MockBluez::NoReply,
                     MockBluez::ReleaseDuringRegister}) {
      bluez.mode = mode;
      auto why = bounded([&] { return backend.start_or_update(frame, 1, "hci7"); });
      CHECK(!why.empty() && !backend.active());
      CHECK(why.find("synthetic private data") == std::string::npos);
      if (mode == MockBluez::Busy) CHECK(why.find("NotPermitted") != std::string::npos);
      if (mode == MockBluez::InvalidLength) CHECK(why.find("InvalidLength") != std::string::npos);
      eventually([&] { return bluez.last_client_gone(); });
    }
    bluez.mode = MockBluez::UnregisterNoReply;
    CHECK(backend.start_or_update(frame, 1, "hci7").empty());
    CHECK(bounded([&] { backend.stop(); return std::string{}; }).empty());
    CHECK(!backend.active());
    eventually([&] { return bluez.last_client_gone(); });
    bluez.mode = MockBluez::Normal;
    {
      BluezBackend other;
      CHECK(backend.start_or_update(frame, 2, "hci7").empty());
      CHECK(other.start_or_update(frame, 2, "hci7").empty());
      backend.stop();
      CHECK(!backend.active() && other.active());
      other.stop();
      eventually([&] { return bluez.last_client_gone(); });
    }
    CHECK(backend.start_or_update(frame, 2, "hci7").empty());
    bluez.drop_name();
    eventually([&] { return !backend.active(); });
    CHECK(!backend.start_or_update(frame, 3, "hci7").empty());
    CHECK(!backend.active());
  }
  g_test_dbus_down(bus);
  g_object_unref(bus);
  fs::remove_all(root);
  std::cout << "BluezBackend isolated tests passed (no physical radio/OpenHD)\n";
}