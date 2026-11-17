# OpenDroneID dependency

- Official upstream: https://github.com/opendroneid/opendroneid-core-c
- Pinned gitlink: `849c03adbe0cdd68997a462fafab397edcfe53f3`.
- License: Apache-2.0; the complete upstream LICENSE and original source
  copyright/SPDX notices are retained in the submodule. Include that LICENSE
  with binary/source redistribution incorporating this optional library.
- This repository builds only the unmodified `libopendroneid/opendroneid.c`
  codec as a static library. No upstream transmitters, examples or GPS sources
  are built or executed. The default upstream two BasicID slots are retained.
- Initialize the submodule recursively in a standalone checkout. There is no
  dependency on the parent workspace, radioHub, its fork, or its modified SHA.
- The radioHub revision examined during development is not present in the
  official repository; it was deliberately NOT used as the dependency pin.