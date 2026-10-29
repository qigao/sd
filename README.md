# sd

独立的原生生图、视频生成前端与服务工程。推理使用未修改的
[stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp)，本项目负责后续的
HTTP 服务、模型/session 管理、任务 JSON 和工作流编排。

sd.cpp 的模型实现、计算后端和部署选项保持上游语义。KFCore 将作为可选能力提供方接入，
不再承载这个应用的主工程；ONNX/TensorRT 是可选工作流节点，不是使用 sd.cpp 的条件。

## 当前范围

当前阶段提供上游依赖、构建和验证基础：

- 通过 Git 子模块固定 sd.cpp，保留其原有 `SD_*` / `GGML_*` 构建选项与安装规则。
- 提供静态 C API 和共享库/动态 GGML/CLI/server 两种 CPU 验证配置。
- 使用实际 C API、设备查询、上游 CLI/server 和无模型 HTTP 请求进行验证。
- 测试使用固定版本 TinyTest；生产依赖不包含 Python 或完整 Salts SDK。

**本项目自有的 HTTP 服务、模型/session 管理器、任务协议和工作流执行器尚未实现。**
目前构建出的 `sd-server` 是上游服务，其模型加载、生成和任务接口遵循上游文档。
当前阶段也没有 CatV2TON 网络实现、GPU 推理结果或性能结论。

此前 [KFCore Draft PR #356](https://github.com/qigao/KFCore/pull/356) 的探索代码保留在原处，
应用主线转到本仓库；这里不复制另一套推理引擎，也不迁入尚未完成的 CatV2TON 计算。
职责划分和后续实现顺序见 [架构说明](docs/architecture.md)。

## 构建与测试

以下命令从本仓库根目录执行，需要 CMake 3.21 或更新版本、Ninja 和 C/C++ 编译器。
`cpu-shared-server` 的 HTTP 测试另需 Python 3.8 或更新版本，仅作为测试客户端，
不改变服务端的原生运行方式。

初始化 CPU 验证所需的子模块：

```sh
git submodule update --init vendor/stable-diffusion.cpp vendor/salts
git -C vendor/stable-diffusion.cpp submodule update --init ggml
```

配置、构建完整构建图，再通过 CTest 执行：

```sh
cmake --preset cpu-release
cmake --build --preset cpu-release
ctest --preset cpu-release

cmake --preset cpu-shared-server
cmake --build --preset cpu-shared-server
ctest --preset cpu-shared-server
```

| Preset | 选择的配置 | 验证目的 |
| --- | --- | --- |
| `cpu-release` | 静态 sd.cpp、CPU、C API 测试 | 公开 API 的链接、初始化和无权重行为 |
| `cpu-shared-server` | 共享 sd.cpp/GGML、动态 CPU backend、RPC、`sd-cli`、`sd-server` | 共享/动态依赖、真实入口程序及无模型 HTTP 行为 |

两种配置都是明确选择的 CPU 验证组合，不限制其他部署。动态 CPU 配置使用
`GGML_NATIVE=OFF`；`cpu-shared-server` 为测试关闭 `SD_SERVER_BUILD_FRONTEND`，
不需要构建 Web bundle。消费者可以通过上游选项选择其他硬件、链接方式、媒体功能或 Web UI。
需要的 SDK 和嵌套子模块遵循固定版本的
[上游构建说明](https://github.com/leejet/stable-diffusion.cpp/blob/77b72a2efed78f55de26892c7e909cee001e8b83/docs/build.md)。

`SD_APP_BUILD_TESTS` 默认关闭；上述验证 presets 显式开启它。`vendor/salts` 仅供这些测试
编译 TinyTest 使用，不是应用运行依赖。测试不下载模型权重；通过构建或设备枚举不能证明
某个模型、GPU 或远程 RPC 设备已通过推理验证。实际运行结果以对应提交的 CI 为准。

## 运行已有的上游 HTTP 服务

完成 `cpu-shared-server` 构建后，可以先不加载模型启动服务：

```sh
mkdir -p models
./build/cpu-shared-server/bin/sd-server \
  --listen-ip 127.0.0.1 \
  --listen-port 1234 \
  --model-dir ./models
```

在另一个终端查询模型状态与 recipes：

```sh
curl --fail http://127.0.0.1:1234/sdcpp/v1/models/status
curl --fail http://127.0.0.1:1234/sdcpp/v1/recipes
```

这是上游已存在的服务能力。模型文件需要另行准备，并按对应 recipe 选择兼容组件。
上游 API 已有模型文件列表、加载/卸载、图像/视频生成、任务查询和取消；完整字段与行为见
[server API](https://github.com/leejet/stable-diffusion.cpp/blob/77b72a2efed78f55de26892c7e909cee001e8b83/examples/server/api.md)。
该测试配置不构建嵌入式 Web UI。上游还支持用 `--serve-html-path` 提供外部前端文件，
具体方式见 [server README](https://github.com/leejet/stable-diffusion.cpp/blob/77b72a2efed78f55de26892c7e909cee001e8b83/examples/server/README.md)。

## 上游同步与依赖

sd.cpp 的根 gitlink 是版本事实源，默认 GGML 跟随该 sd.cpp 提交记录的子模块。
同步时选择并暂存新的 gitlink，更新对应依赖和公开 API 适配，再执行 configure/build/CTest；
不在 vendor 内维护本地补丁。具体流程见 [上游同步](docs/upstream-sync.md)。

当前依赖基线：

| 依赖 | 固定提交 | 用途 |
| --- | --- | --- |
| [sd.cpp](https://github.com/leejet/stable-diffusion.cpp/tree/77b72a2efed78f55de26892c7e909cee001e8b83) | `77b72a2efed78f55de26892c7e909cee001e8b83` | 原生生成引擎及上游 CLI/server |
| [GGML](https://github.com/leejet/ggml/tree/643e8d970cd2d926a0074f5cb12a68b38078157a) | `643e8d970cd2d926a0074f5cb12a68b38078157a` | sd.cpp 默认计算依赖 |
| [Salts](https://github.com/qigao/salts/tree/209119b6c54de6d937ded77b782948c94498c523) | `209119b6c54de6d937ded77b782948c94498c523` | 测试用 TinyTest 源码 |

第三方来源与许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
