# Third-party sources

Inference and test dependencies remain in Git submodules. Native frontend source
is maintained in `apps/` with its original license. No model weights or generated
inference artifacts are included. The former Node-based Web UI has been removed.

| Source | Initial revision | Role | License source |
| --- | --- | --- | --- |
| [leejet/stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp/tree/77b72a2efed78f55de26892c7e909cee001e8b83) | `77b72a2efed78f55de26892c7e909cee001e8b83` | Unmodified engine and source of native code in `apps/` | Upstream MIT; copied license at [apps/LICENSE](apps/LICENSE), Copyright (c) 2023 leejet |
| [leejet/ggml](https://github.com/leejet/ggml/tree/643e8d970cd2d926a0074f5cb12a68b38078157a) | `643e8d970cd2d926a0074f5cb12a68b38078157a` | Default GGML selected by sd.cpp | Upstream MIT and bundled notices |
| [qigao/salts](https://github.com/qigao/salts/tree/209119b6c54de6d937ded77b782948c94498c523) | `209119b6c54de6d937ded77b782948c94498c523` | Test-only TinyTest | Upstream Apache-2.0 `LICENSE` and applicable notices |

The root gitlinks are the executable revision authority for sd.cpp and test-only
Salts. sd.cpp's gitlink selects its default GGML. The versions above record the
initial baseline and must be updated when these sources change.

Native source/header files and the recipe template were initially copied without
semantic edits. The owned server's generated-Web-UI include path has since been
removed. Application CMake provides independent targets and dependency wiring.
See [native frontend migration](docs/frontend-migration.md) for the source mapping.

CHttp is currently a researched integration target, not a linked dependency. Its
reviewed revision, dependencies and API limits are recorded in the
[service design](docs/chttp-service-design.md); this notice does not imply that
its service runtime or SDK has already been imported.

Explicit upstream system/custom GGML and additional codec/accelerator options
retain upstream semantics. Their dependencies require matching source, notices
and qualification; the bundled CPU profiles do not certify every configuration.

The build integration and API tests originated in
[KFCore draft PR #356](https://github.com/qigao/KFCore/pull/356). This repository
continues sd application work independently and does not copy KFCore runtime or
experimental CatV2TON conditioning into the engine.
