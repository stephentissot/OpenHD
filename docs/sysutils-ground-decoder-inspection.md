# Ground decoder inspection — 2026-11-17

Read-only inspection through WSL Ubuntu-22.04. No SysUtils source changes,
daemon execution, target connection or service changes.

## Exact source locations

Inspected tree: `/opt/openhd-dev/sysutils-source`, clean Git worktree,
revision `e89ce3d8541189c145c949cd04dc5b5f1d62cc83`.
The identifiers `officialSysUtils`, `launchQOpenHD` and `olderPi` do not occur
in this tree; do not confuse historical names with this revision's functions.

All paths below are relative to that WSL tree:

| Source | Function / evidence |
| --- | --- |
| `src/openhd_sys_utils.cpp:403–405` | Startup calls `start_openhd_services_if_needed()`, then `start_ground_video_if_needed()` (with hostname application immediately before them). |
| `src/sysutil_video.cpp:429–446` | `start_qopenhd_if_needed()` reloads systemd and starts `qopenhd.service`; it does not launch a player itself. |
| `src/sysutil_video.cpp:609–655` | `start_openhd_services_if_needed()` starts core, and QOpenHD in Ground mode unless Glide is selected. |
| `src/sysutil_video.cpp:546–585` | `start_ground_video_if_needed()` separately starts the Raspberry Pi decoder; only Pi4/CM4 FKMS waits for a client request. |
| `src/sysutil_video.cpp:383–399` | `start_video_process()` forks a process group and executes the pipeline through `/bin/sh -c`. |
| `src/sysutil_video.cpp:107–117` | `pi_video_pipeline()` constructs the actual non-Pi5 pipeline. |
| `src/sysutil_video.cpp:163–185` | `is_rpi_platform()` and `is_pi4_fkms()`; old Pi has no Pi4-FKMS startup exception. |
| `misc/platforms.json:100–106` | Raspberry Pi model fallback selects `X_PLATFORM_TYPE_RPI_OLD`; applies to Zero 2 after earlier Pi4/5/CM4 checks. |
| `inc/sysutil_config.h:39–102`, `src/sysutil_config.cpp:36–38` | Configuration fields and `/usr/local/share/OpenHD/SysUtils/config.json`. No standalone-decoder disable field. |
| `src/sysutil_video.cpp:233–243,617–620` | `disable_openhd_service=true` stops/disables core and skips the UI-start section; it is not a decoder-only switch. The separate subsequent decoder-start call is not gated by it. |
| `src/sysutil_video.cpp:662–716` | `handle_video_request()` accepts `action=stop` independently of core/UI, plus start/restart. |
| `src/openhd_sys_utils.cpp:37–38,293` | UNIX stream socket `/run/openhd/openhd_sys.sock`, request dispatch. |
| `old/h264_decode.sh:10–12` | Historical `execute_fpvVideoDecode()` uses `fpv_video0.bin`; not the current C++ launch entry point. |
| `CMakeLists.txt:182` | SysUtils DEB declares only `init-system-helpers, lsb-base`; decoder runtime dependencies are not guaranteed by this package. |

## Zero 2 / older-Pi execution and requirements

Default H.264 path, expressed as pipeline stages:

`gst-launch-1.0 -q` → `udpsrc port=5600` with RTP H264/video/90000 caps →
`rtph264depay` → `video/x-h264,stream-format=byte-stream` → `fdsink` →
player reading `/dev/stdin`.

Player selection: executable `/usr/local/bin/openhd_pi_vid`, otherwise
`fpv_video0.bin` resolved through PATH. No `rpi-player` executable is referenced
in current SysUtils source. There is no `h264parse`, `avdec_h264`, `omxh264dec`,
`v4l2h264dec` or GStreamer video sink in the old-Pi pipeline: decoding/display
are delegated to the separate player.

Required GStreamer elements/files on Bullseye ARMhf:

- `gst-launch-1.0`: package `gstreamer1.0-tools`.
- `udpsrc`: `libgstudp.so`; `rtph264depay`: `libgstrtp.so`, normally
  `gstreamer1.0-plugins-good`.
- `fdsink` and implicit `capsfilter`: `libgstcoreelements.so`,
  `libgstreamer1.0-0` (ownership verified in the local ARMhf rootfs).
- Plugin directory: `/usr/lib/arm-linux-gnueabihf/gstreamer-1.0/`.
- `/bin/sh`, `/dev/stdin`, and one working player plus its own runtime libraries
  and display/device access. Player ELF dependencies were not inspected here.

The build rootfs contains `libgstcoreelements.so` but the search found neither
`libgstudp.so`, `libgstrtp.so`, `fpv_video0.bin` nor `openhd_pi_vid` under its
`usr` tree. This is build-rootfs evidence, not an inventory of the live Ground.

There is **no persistent decoder-only disable setting** in this revision.
A newline-terminated `{"type":"sysutil.video.request","action":"stop"}`
request over the socket stops the Pi decoder process group without stopping
core or QOpenHD. It is not persistent: startup or a later start/restart request
can launch it again. Disabling/masking `openhd-video.service` does not suppress
this direct old-Pi child process. No stop request was sent during inspection.

## Local Ground backup correction

The preflight and rollback lists now include the actual Qt settings file
`/root/.config/OpenHD/QOpenHD.conf` and its `/home/openhd` equivalent,
while retaining the old uppercase-QOpenHD paths. Only the specific file is
removed/restored in the OpenHD organization directory, preserving sibling files.
The fixture uses both scripts' actual path lists, archives/restores both files,
and checks that an unrelated sibling survives. Migration fixtures, Bash syntax,
editor diagnostics and local tool manifest hashes pass.

Existing target backups and the previously generated/uploaded bundle are
unchanged. Refresh the bundle/checksums and capture the missing UI file before
any deployment; changing the scripts does not repair an existing backup.