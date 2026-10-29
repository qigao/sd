# sd

独立的原生生图、视频生成前端与服务工程。本仓库维护可编辑的 CLI、HTTP 服务和 Web UI；
推理继续使用未修改的 [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp)。
KFCore、ONNX/TensorRT 按需作为基础能力或工作流节点接入，不是使用 sd.cpp 的条件。

## 当前源码与功能

| 目录 | 职责 |
| --- | --- |
| `apps/cli/` | 本地 CLI，生成程序 `sd-app-cli` |
| `apps/server/` | 本地 HTTP 服务，生成程序 `sd-app-server`；模型目录、recipe、加载/卸载、生成任务与取消 |
| `apps/common/` | 两个原生入口共用的参数解析、媒体 IO、日志与资源管理 |
| `frontend/` | 可直接修改和热更新的 Vue/TypeScript Web UI |
| `vendor/stable-diffusion.cpp/` | 未修改的模型、采样、设备与计算实现；其 GGML 跟随上游 gitlink |
| `tests/` | 真实 C API、入口程序与 HTTP 行为测试 |

原生前端从 sd.cpp 固定提交完整迁入，Web UI 从对应 `sdcpp-webui` 提交迁入，保留原有
协议和许可证。应用 target 与上游 `sd-cli`、`sd-server` 名称不同，允许同时构建。
内置 recipes 仍读取 vendor 的单一目录；用户 recipes 使用现有模型根目录机制。
详细来源和修改范围见 [前端迁移记录](docs/frontend-migration.md)。

已有的单次生成任务与新的跨节点 JSON 工作流是不同层次：当前保留原有生成、模型管理和
任务 API，尚未实现跨节点工作流执行器、KFCore 节点或 CatV2TON 网络。
[架构说明](docs/architecture.md) 记录这些边界。

## 本地开发

原生开发需要 Git、CMake 3.21+、Ninja、C/C++17 编译器；HTTP 测试需要 Python 3.8+。
Web UI 使用 Node.js 20+ 与 pnpm **10.15.1**。Python 只用作测试客户端，不参与生产推理。

初始化 CPU 开发和测试所需依赖：

```sh
git submodule update --init vendor/stable-diffusion.cpp vendor/salts
git -C vendor/stable-diffusion.cpp submodule update --init ggml

cmake --preset dev-cpu
cmake --build --preset dev-cpu
ctest --preset dev-cpu
```

`dev-cpu` 构建 Debug 程序并生成 `build/dev-cpu/compile_commands.json`。在 IDE 中选择
该 preset 和 `sd-app-server` 目标即可调试本仓库的原生服务源码。

从仓库根目录启动本地服务：

```sh
mkdir -p models
./build/dev-cpu/bin/sd-app-server \
  --listen-ip 127.0.0.1 --listen-port 1234 --model-dir ./models
```

Windows 使用 Visual Studio x64 开发者终端构建，并运行
`build\dev-cpu\bin\sd-app-server.exe`。完整的 Windows、Linux/macOS 和 CUDA 步骤见
[本地开发说明](docs/local-development.md)。

在另一个终端启动可热更新的 Web UI：

```sh
cd frontend
pnpm install --frozen-lockfile
pnpm dev
```

访问 `http://127.0.0.1:5173`。UI 的 Backend base URL 留空时，`/sdcpp` 请求通过 Vite
代理转到 `http://127.0.0.1:1234`；启动 Vite 前可通过 `SD_APP_BACKEND_URL` 明确选择其他
后端地址。开发端口固定并启用 `strictPort`，不自动换端口。

服务不加载权重也能提供模型目录与状态：

```sh
curl --fail http://127.0.0.1:1234/sdcpp/v1/models/status
curl --fail http://127.0.0.1:1234/sdcpp/v1/recipes
```

实际生成需要准备模型并选择兼容的组件。构建与测试不会自动下载模型权重。

## 单 HTML 运行方式

在 `frontend/` 执行：

```sh
pnpm type-check
pnpm build
```

停止之前占用 1234 的服务进程，再从仓库根目录运行：

```sh
./build/dev-cpu/bin/sd-app-server \
  --listen-ip 127.0.0.1 --listen-port 1234 --model-dir ./models \
  --serve-html-path ./frontend/dist/index.html
```

访问 `http://127.0.0.1:1234`，页面和 API 同源。原生服务在每次请求时读取 HTML，重建
前端后刷新即可更新页面，无须重新编译服务，也不需要生产 Node.js 进程。

## 构建配置与验证

| Preset | 用途 | 配置的测试 |
| --- | --- | --- |
| `dev-cpu` | 本地 Debug、编译数据库、迁入的 CLI/server | 6 项 CTest，包括实际 HTTP 与外部 HTML 更新 |
| `dev-cuda` | 本地 Debug，显式启用上游 CUDA | 同一应用测试；另需 CUDA SDK 和 CUTLASS，须在目标机器验证 |
| `cpu-release` | 静态库 C API 基线，关闭应用入口 | 1 项 CTest |
| `cpu-shared-server` | 共享 sd.cpp/GGML、动态 CPU/RPC、本地与上游入口共存 | 11 项 CTest |

这些是明确选择的构建组合。其他硬件、媒体功能、内存放置和部署方式继续使用上游
`SD_*` / `GGML_*` 选项，接入层不强制重写它们。`SD_APP_BUILD_CLI`、
`SD_APP_BUILD_SERVER` 默认开启；`SD_APP_BUILD_TESTS` 默认关闭，由验证 presets 开启。
Salts 只提供测试用 TinyTest，不是应用 SDK 依赖。

CI 完整构建所选图，并验证前端的 frozen-lockfile 安装、类型检查和构建。每项验证以对应
提交的运行结果为准；CPU、无权重 HTTP 和构建成功不等于真实模型、GPU 或远程 RPC 已通过。

## 同步与后续开发

sd.cpp 的 gitlink 是引擎版本事实源；默认 GGML 跟随该提交。迁入的应用代码在 `apps/`
与 `frontend/` 中维护。更新引擎时检查公开 API、参数映射和 recipe 语义，并按
[同步流程](docs/upstream-sync.md) 重新配置、构建和测试。

此前 [KFCore Draft PR #356](https://github.com/qigao/KFCore/pull/356) 保留探索记录，sd 应用
主线在本仓库继续。模型、输出、本机配置和构建产物均不提交。
第三方来源与许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
