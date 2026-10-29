# Third-party sources

Inference and test dependencies remain in Git submodules. The native and web
frontends have been imported as editable application source, with their original
licenses retained. No model weights or generated inference artifacts are included.

| Source | Initial revision | Role | License source |
| --- | --- | --- | --- |
| [leejet/stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp/tree/77b72a2efed78f55de26892c7e909cee001e8b83) | `77b72a2efed78f55de26892c7e909cee001e8b83` | Unmodified inference engine; source of migrated CLI/common/HTTP code in `apps/` | Upstream MIT license; copied frontend license at [apps/LICENSE](apps/LICENSE), Copyright (c) 2023 leejet |
| [leejet/ggml](https://github.com/leejet/ggml/tree/643e8d970cd2d926a0074f5cb12a68b38078157a) | `643e8d970cd2d926a0074f5cb12a68b38078157a` | Default GGML submodule selected by sd.cpp | Upstream MIT license and bundled third-party notices |
| [qigao/salts](https://github.com/qigao/salts/tree/209119b6c54de6d937ded77b782948c94498c523) | `209119b6c54de6d937ded77b782948c94498c523` | TinyTest source and headers, only when integration tests are enabled | Upstream Apache-2.0 `LICENSE` and applicable notices |
| [leejet/sdcpp-webui](https://github.com/leejet/sdcpp-webui/tree/fa93429c47c924054e6a9c14f8d7b8e06f57a0ec) | `fa93429c47c924054e6a9c14f8d7b8e06f57a0ec` | Editable Web UI source in `frontend/` | [frontend/LICENSE](frontend/LICENSE), MIT, Copyright (c) 2026 leejet |

The root gitlinks are the executable revision authority for sd.cpp and Salts;
sd.cpp's own gitlink selects its default bundled GGML. The versions above record
this baseline and must be updated when those sources change.

The native frontend source/header files and recipe header template were copied
without semantic edits. Application CMake files provide independent targets and
dependency wiring. The Web UI retains its original lockfile; local changes add
the package identity, development proxy/ports and development documentation.
See [frontend migration](docs/frontend-migration.md) for the source mapping.

Explicit upstream system/custom GGML and additional codec/accelerator options
retain upstream configuration semantics. Their dependencies require their own
matching source, notices and qualification; they are not certified by the
bundled CPU test profiles.

The build integration and API tests originated in the user's exploratory
[KFCore draft PR #356](https://github.com/qigao/KFCore/pull/356). This repository
continues the sd-specific work independently. It does not copy KFCore runtime
implementation or experimental CatV2TON conditioning into the engine.
