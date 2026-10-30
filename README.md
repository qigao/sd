# sd

独立的原生图像/视频生成服务工程。推理使用未修改的
[stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp)，服务重构以 CHttp 为基础，
目标是让客户端通过 HTTP/2 提交任务、查询状态和获取结果，通过 WebSocket 接收任务事件。

本应用已移除 Node.js、pnpm、Vue/Vite 前端源码与对应 CI。
本地 httplib 基线默认拒绝任意跨源 Origin、Referer 和 DNS-rebinding Host，
不再对任意网页开放带凭据的 CORS；这不是正式的 CHttp 身份认证。KFCore、ONNX/TensorRT 按实际
计算节点需要接入，不是使用 sd.cpp 的条件。

## 当前实现与重构方向

| 目录/能力 | 当前状态 |
| --- | --- |
| `apps/cli/` | 可编辑的原生 `sd-app-cli` |
| `apps/common/` | 参数解析、媒体 IO、日志与资源管理 |
| `apps/server/` | 迁入的模型与任务基线，当前 `sd-app-server` 仍使用原有 httplib 路由 |
| `vendor/stable-diffusion.cpp/` | 未修改的推理引擎，GGML 跟随其 gitlink |
| CHttp HTTP/2、WS、认证与业务 Service | 已进行固定源码审查，接入契约和实施次序见下方设计；尚未链接进应用 |
| JSON Workflow V1 | 原生 `/sdcpp/v1/workflows`：严格 JSON、模型实例+修订绑定、生成 DAG、取消与有界结果；仍基于 httplib，仅 loopback 开发使用 |
| KFCore 节点、CatV2TON 网络 | 尚未实现；拒绝未知节点类型 |

[Workflow V1 合同与测试层次](docs/workflow-v1.md) 说明已实现的本地工作流子集、控制依赖、参数范围、任务所有权与明确未实现的媒体边。  
原生输出的 C 结果结构、像素缓冲和 ADetailer 元数据采用异常安全所有权转移，
无权重 CTest 包含 native image/video owner 接管测试；真实权重故障资格仍未完成。

本地原生任务保留表另以每任务 128 MiB、全局 256 MiB 的**编码媒体**
预算管理 Completed 结果，并在 TTL 清理时退回配额；这不包括 GPU/临时编码/
HTTP reader 或用户级存储预算，正式集成仍需 CHttp 对象/会话配额。

图像、视频及 Workflow 提交统一使用原子 `AsyncJobManager` 接纳与不可变 202 回执，
默认 CI 的原生任务状态测试覆盖容量、并发接纳、取消/完成竞争与 TTL 回收。
正式的 HTTP/2 用户身份、幂等提交及产物存储仍需 CHttp 接入。

Workflow V1 真实权重验收可按 [本地 manifest 示例](tests/workflow-real-model-manifest.example.json)\n配置 `SD_APP_WORKFLOW_TEST_MANIFEST`。默认 CTest 不下载权重、不会把纯解析通过当成模型计算通过。\n\n[CHttp 服务设计](docs/chttp-service-design.md) 明确认证、任务提交、状态、取消、产物下载、
WS 订阅、资源上限和共享 Service 能力缺口。文中的新接口是待实现契约，不是当前路由。
原有 `/sdcpp/v1/*` 无模型基线测试不能证明新服务的认证或多用户隔离已完成。

## 本地原生开发

需要 Git、CMake 3.21+、Ninja、C/C++17 编译器；HTTP 测试需要 Python 3.8+。
Python 只用于测试，不参与生产推理。构建当前原生程序不需要 Node.js。

```sh
git submodule update --init vendor/stable-diffusion.cpp vendor/salts
git -C vendor/stable-diffusion.cpp submodule update --init ggml

cmake --preset dev-cpu
cmake --build --preset dev-cpu --parallel 2
ctest --preset dev-cpu
```

`dev-cpu` 生成 Debug 程序和 `build/dev-cpu/compile_commands.json`。IDE 使用该 preset，
选择 `sd-app-server` 或 `sd-app-cli` 调试目标。

从仓库根目录启动当前推理接口基线：

```sh
mkdir -p models
./build/dev-cpu/bin/sd-app-server \
  --listen-ip 127.0.0.1 --listen-port 1234 --model-dir ./models
```

可直接观察实际 HTTP 接口：

```sh
curl --fail http://127.0.0.1:1234/sdcpp/v1/models/status
curl --fail http://127.0.0.1:1234/sdcpp/v1/recipes
```

模型根目录与内置 recipes 机制保持原有语义；实际生成需准备兼容权重。当前基线服务未实现
下文设计的身份/任务权限，不作为新的 CHttp 对外服务验收结果。
Windows、CUDA 和调试步骤见 [本地开发](docs/local-development.md)。

## 构建与验证

| Preset | 用途 | 当前测试 |
| --- | --- | --- |
| `dev-cpu` | Debug、编译数据库、本地 CLI/server | 6 项 CTest |
| `dev-cuda` | 显式启用上游 CUDA | 同一应用测试，另需 SDK、CUTLASS 和目标设备验证 |
| `cpu-release` | 静态库 C API 基线 | 1 项 CTest |
| `cpu-shared-server` | 共享 sd.cpp/GGML、动态 CPU/RPC、本地与上游入口共存 | 11 项 CTest |

CI 的两个 CPU 构建组合固定 `GGML_NATIVE=OFF`，避免跨 runner 指令集差异与复用 native 编译缓存造成 SIGILL；本地 `dev-cpu` 仍保留 `GGML_NATIVE=ON`。
CI 只保留原生构建矩阵，完整构建所选图、执行 CTest、核对精确提交，使用编译缓存并取消
同一 PR 的旧运行。当前测试不下载模型，不证明 GPU、真实生成、远程 RPC 或未来 CHttp 接入。

这些 preset 是验证组合。上游其他硬件、媒体编码、内存放置和部署选项继续保持可配置。
`SD_APP_BUILD_CLI`、`SD_APP_BUILD_SERVER` 默认开启；`SD_APP_BUILD_TESTS` 默认关闭。
测试用 Salts 只提供 TinyTest；它不是尚未接入的 CHttp 所需完整 SDK。

## 开发依据

- [CHttp 服务设计与源码审查](docs/chttp-service-design.md)
- [架构与实现边界](docs/architecture.md)
- [原生前端迁移记录](docs/frontend-migration.md)
- [上游同步](docs/upstream-sync.md)
- [第三方来源](THIRD_PARTY_NOTICES.md)

此前 [KFCore Draft PR #356](https://github.com/qigao/KFCore/pull/356) 保留探索记录，sd 应用
主线在本仓库。模型、输出、本机配置和构建产物不提交。
