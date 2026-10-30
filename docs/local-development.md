# 本地原生开发

应用源码位于 `apps/`，程序为 `sd-app-cli`、`sd-app-server`。Node.js/pnpm/Vue/Vite
应用链已删除。当前服务是迁入的推理接口基线，仍使用原有 HTTP 实现及 `/sdcpp/v1/*`；
新的 CHttp HTTP/2、WS 与认证接口按 [服务设计](chttp-service-design.md) 实施，尚未注册。

## 工具与依赖

| 工具 | 用途 |
| --- | --- |
| Git、CMake 3.21+、Ninja、C/C++17 编译器 | 当前原生构建与调试 |
| Python 3.8+ | CTest HTTP 客户端 |
| 兼容的 CUDA Toolkit 与 host compiler | 仅 CUDA preset 需要 |

CHttp 自身当前要求 CMake 3.25+ 及其明确的 SDK 依赖；正式接入时一起更新本仓库构建契约，
不把当前 TinyTest-only 的 Salts 子模块当成完整 CHttp 依赖。

Linux 使用本机 C/C++ 工具链。Windows 使用 Visual Studio x64 开发者终端，确认 `cl`、
`link`、Ninja 可用。macOS 可以使用 Apple Clang 进行 CPU 开发，Metal 配置遵循上游选项。
本机工具链路径和设备架构放在被忽略的 `CMakeUserPresets.json` 或显式配置参数中。

初始化当前 CPU 开发与测试依赖：

```sh
git submodule update --init vendor/stable-diffusion.cpp vendor/salts
git -C vendor/stable-diffusion.cpp submodule update --init ggml
```

无需初始化上游 Web UI 子模块。测试与构建不自动下载模型权重。

## CPU 构建与调试

```sh
cmake --preset dev-cpu
cmake --build --preset dev-cpu --parallel 2
ctest --preset dev-cpu
```

IDE 选择 `dev-cpu`，使用 `build/dev-cpu/compile_commands.json`，调试 `sd-app-server`
或 `sd-app-cli`。修改 `apps/` 后执行完整 build，不修改 vendor 的模型/算子实现。

Linux/macOS 从仓库根目录启动当前服务：

```sh
mkdir -p models
./build/dev-cpu/bin/sd-app-server --listen-ip 127.0.0.1 --listen-port 1234 --model-dir ./models
```

Windows PowerShell：

```powershell
New-Item -ItemType Directory -Force models | Out-Null
.\build\dev-cpu\bin\sd-app-server.exe --listen-ip 127.0.0.1 --listen-port 1234 --model-dir ./models
```

查询当前模型状态：

```sh
curl --fail http://127.0.0.1:1234/sdcpp/v1/models/status
curl --fail http://127.0.0.1:1234/sdcpp/v1/recipes
```

服务可在没有权重时提供目录与状态，生成仍需加载兼容模型。当前基线没有新服务所需的
身份与任务归属校验，只用于本地推理接入验证；不得将其响应当作 CHttp 认证已实现。

## CPU runner 指令集与缓存

CI 的 `cpu-release` / `cpu-shared-server` 显式使用 `GGML_NATIVE=OFF`：
GitHub runner 可能有不同的 CPU ISA，不能用带 `-march=native` 的编译缓存跨机器部署。
CI 配置步骤会读取实际 `CMakeCache.txt` 强制验证这一约束，不跳过
`sd_list_devices()` 等真实 C API 测试。

本机 `dev-cpu` 与 `dev-cuda` 保留 `GGML_NATIVE=ON` 以使用本机原生 CPU 优化。
用户仍可通过 `-DGGML_NATIVE=OFF/ON` 显式覆盖。此限制仅用于两个
共享缓存的 CI CPU 配置，未删除或强制关闭上游其他 SD/GGML 设备选项。

## CUDA

`dev-cuda` 启用上游 `SD_CUDA` / `GGML_CUDA`。先准备匹配的 CUDA 工具链及 CUTLASS：

```sh
git -C vendor/stable-diffusion.cpp/ggml submodule update --init --recursive src/ggml-cuda/vendors/cutlass
cmake --preset dev-cuda
cmake --build --preset dev-cuda
ctest --preset dev-cuda
```

缺少 SDK 或 headers 时修复对应环境，不以 CPU 回退作为 CUDA 通过。硬件、真实模型输出与
性能须单独验收；选项见 [上游构建说明](https://github.com/leejet/stable-diffusion.cpp/blob/77b72a2efed78f55de26892c7e909cee001e8b83/docs/build.md)。

## 下一阶段的客户端验收

CHttp 接入完成后，使用原生 CHttp Client 或支持 HTTP/2 的测试客户端验证认证、提交、
状态和产物，再通过 CHttp WS Client 验证有序订阅、权限与慢消费者关闭。测试客户端不
创建第二套服务状态，不依赖 Node 进程。具体验收用例以服务设计中的状态机和权限表为准。
