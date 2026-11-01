# License

This repository, **rrc** (Reticulum Relay Chat client on spangap, with a
browser window and an on-device LVGL app), is released under the **Apache
License, Version 2.0**.

Full license text: <https://www.apache.org/licenses/LICENSE-2.0>

Copyright (c) 2026 by reticulous project contributors.

## Third-party software

### Vendored in this repository

None.

### Build-time dependencies

Declared in `esp-idf/idf_component.yml` and `browser/package.json`:

| Component / package | Source | License |
|---|---|---|
| ESP-IDF (platform) | espressif/esp-idf | Apache-2.0 |
| Browser peer deps (Vue, Quasar, Pinia, vue-router) | npm | MIT |

The RRC protocol is specified by kc1awv (<https://rrc.kc1awv.net>), with the
extensions of the reference hub `kc1awv/rrcd`. This implementation is
independent; no RRC client or hub source code is incorporated.

The host test vectors (`esp-idf/test/host/vectors.h`) are generated with
`cbor2` (MIT).
