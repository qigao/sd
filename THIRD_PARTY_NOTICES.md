# Third-party sources

This repository keeps third-party source in Git submodules. Their original
copyright, license and notice files remain in those checkouts. No model weights
or generated inference artifacts are included.

| Source | Initial revision | Role | License source |
| --- | --- | --- | --- |
| [leejet/stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp/tree/77b72a2efed78f55de26892c7e909cee001e8b83) | `77b72a2efed78f55de26892c7e909cee001e8b83` | Unmodified inference engine and baseline native CLI/HTTP server | Upstream MIT license and bundled third-party notices |
| [leejet/ggml](https://github.com/leejet/ggml/tree/643e8d970cd2d926a0074f5cb12a68b38078157a) | `643e8d970cd2d926a0074f5cb12a68b38078157a` | Default GGML submodule selected by sd.cpp | Upstream MIT license and bundled third-party notices |
| [qigao/salts](https://github.com/qigao/salts/tree/209119b6c54de6d937ded77b782948c94498c523) | `209119b6c54de6d937ded77b782948c94498c523` | TinyTest source and headers, only when integration tests are enabled | Upstream Apache-2.0 `LICENSE` and applicable notices |

The root gitlinks are the executable revision authority for sd.cpp and Salts;
sd.cpp's own gitlink selects its default bundled GGML. The versions above record
this baseline and must be updated when those sources change.

Explicit upstream system/custom GGML and additional codec/accelerator options
retain upstream configuration semantics. Their dependencies require their own
matching source, notices and qualification; they are not certified by the
bundled CPU test profiles.

The build integration and API tests originated in the user's exploratory
[KFCore draft PR #356](https://github.com/qigao/KFCore/pull/356). This repository
continues the sd-specific work independently. It does not copy KFCore runtime
implementation or experimental CatV2TON conditioning into the engine.
