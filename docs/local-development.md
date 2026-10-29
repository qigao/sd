# 本地开发

本项目的可编辑入口是 `apps/` 中的原生 CLI/HTTP 服务和 `frontend/` 中的 Vue 前端。
两者调用未修改的 sd.cpp 引擎。应用程序名为 `sd-app-cli`、`sd-app-server`，与上游
`sd-cli`、`sd-server` 分开；当前沿用 `/sdcpp/v1/*` 协议，没有新增工作流 API。

## 准备工具与依赖

| 工具 | 用途 |
| --- | --- |
| Git、CMake 3.21+、Ninja、C/C++17 编译器 | 原生构建与调试 |
| Python 3.8+ | CTest 的 HTTP 行为测试客户端 |
| Node.js 20+、pnpm 10.15.1 | 前端开发、类型检查和单 HTML 构建 |
| 兼容的 CUDA Toolkit 与 host compiler | 仅 `dev-cuda` 需要 |

Linux 使用本机 C/C++ 工具链。Windows 在 Visual Studio 的 x64 开发者终端中执行
Ninja presets，先确认 `cl`、`link` 可用；CUDA 开发还需 `nvcc`。macOS 可以使用
Apple Clang 进行 CPU 开发；GPU 配置可按上游说明选择 Metal。CUDA preset 面向具备
兼容 CUDA 工具链的 Windows/Linux 环境。

本机工具链路径和 CUDA 架构不要写进共享 preset。需要覆盖时使用被 Git 忽略的
`CMakeUserPresets.json`，或显式配置参数。先通过 `pnpm --version` 确认版本为 10.15.1。

从仓库根目录初始化 CPU 开发和测试依赖：

```sh
git submodule update --init vendor/stable-diffusion.cpp vendor/salts
git -C vendor/stable-diffusion.cpp submodule update --init ggml
```

`frontend/` 已是本仓库跟踪的应用源码，不需要初始化上游前端子模块。Salts 仅用于测试
中的 TinyTest。模型权重需按使用的 recipe 自行准备，不会被构建或测试自动下载。

## CPU 原生调试

```sh
cmake --preset dev-cpu
cmake --build --preset dev-cpu
ctest --preset dev-cpu
```

`dev-cpu` 使用 Debug 和 Ninja，构建本项目的 CLI/server，并生成
`build/dev-cpu/compile_commands.json`。在 IDE 中选择同一 CMake preset 与
`sd-app-server` 调试目标即可编辑和调试原生 HTTP 源码。修改源码后执行完整 build；
不要在 vendor 内改动模型实现。

先创建一个模型根目录，然后启动未加载模型的服务。Linux/macOS：

```sh
mkdir -p models
./build/dev-cpu/bin/sd-app-server --listen-ip 127.0.0.1 --listen-port 1234 --model-dir ./models
```

Windows PowerShell：

```powershell
New-Item -ItemType Directory -Force models | Out-Null
.\build\dev-cpu\bin\sd-app-server.exe --listen-ip 127.0.0.1 --listen-port 1234 --model-dir ./models
```

服务可以在没有权重时提供 recipe、文件目录和模型状态。生成仍需要先选择兼容组件并
加载模型；无模型时返回明确错误。

## 前端热更新

保留原生服务终端，在另一个终端从仓库根目录执行：

```sh
cd frontend
pnpm install --frozen-lockfile
pnpm dev
```

打开 `http://127.0.0.1:5173`。修改 `frontend/src/` 后由 Vite 热更新。
UI Settings 中的 Backend base URL 留空时，原生 API 请求通过开发代理转到
`http://127.0.0.1:1234`。已保存的非空 base URL 会绕过代理，应按当前开发目标调整。

后端使用其他地址时，在启动 Vite 前设置 `SD_APP_BACKEND_URL`。Linux/macOS：

```sh
SD_APP_BACKEND_URL=http://127.0.0.1:4321 pnpm dev
```

Windows PowerShell：

```powershell
$env:SD_APP_BACKEND_URL = 'http://127.0.0.1:4321'
pnpm dev
```

代理目标必须是没有凭据、query 或 fragment 的绝对 HTTP(S) URL。Vite 仅监听
`127.0.0.1`；开发端口固定为 5173，preview 固定为 4173。端口被占用时直接失败，
不会换端口或选择其他后端。通过 Ctrl+C 分别停止前端和原生服务。

## 单 HTML 构建与本地部署

在 `frontend/` 中执行：

```sh
pnpm type-check
pnpm build
```

产物为 `frontend/dist/index.html`，不包含独立的 Node.js 服务。停止原来占用 1234 的
进程后，从仓库根目录运行原生服务并指定该文件。Linux/macOS：

```sh
./build/dev-cpu/bin/sd-app-server --listen-ip 127.0.0.1 --listen-port 1234 --model-dir ./models --serve-html-path ./frontend/dist/index.html
```

Windows：

```powershell
.\build\dev-cpu\bin\sd-app-server.exe --listen-ip 127.0.0.1 --listen-port 1234 --model-dir ./models --serve-html-path .\frontend\dist\index.html
```

访问 `http://127.0.0.1:1234`，页面与 API 同源。HTML 在请求时重新读取；修改前端后重新
`pnpm build` 并刷新页面即可更新，不需要重新编译原生服务。也可以在 `frontend/` 运行
`pnpm preview` 查看构建产物，它使用 4173 端口及同一后端代理。

## CUDA 开发配置

`dev-cuda` 显式启用上游 `SD_CUDA` / `GGML_CUDA`。先安装匹配的工具链，并初始化
当前 patched GGML 默认需要的 CUTLASS：

```sh
git -C vendor/stable-diffusion.cpp/ggml submodule update --init --recursive src/ggml-cuda/vendors/cutlass
cmake --preset dev-cuda
cmake --build --preset dev-cuda
ctest --preset dev-cuda
```

缺少 SDK 或必要 headers 应先修复环境，不能把配置成功等同于实际 GPU 推理通过。
此配置未声明跨 GPU/驱动的验证结果；硬件资格与真实性能需在目标机器使用真实权重测试。
具体后端选项遵循 [上游构建说明](https://github.com/leejet/stable-diffusion.cpp/blob/77b72a2efed78f55de26892c7e909cee001e8b83/docs/build.md)。

## 源码同步与修改范围

原生服务源码位于 `apps/`，前端源码位于 `frontend/`，可以在本仓库正常修改。
来源版本、许可和本地修改记录见 [第三方说明](../THIRD_PARTY_NOTICES.md)。
引擎升级按 [上游同步](upstream-sync.md) 操作；不要用修改 vendor 来绕过集成差异。
模型、输出、`node_modules`、`dist` 和本机构建目录均不应提交。
