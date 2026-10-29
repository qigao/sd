# 前端源码迁移记录

本次迁移把可修改的原生入口和 Web UI 放入独立应用仓库。模型计算和 GGML 保留在未修改的
sd.cpp 子模块中。源码与上游的关系如下：

| 来源 | 固定提交 | 本地目录 | 迁移范围 |
| --- | --- | --- | --- |
| [sd.cpp examples/common](https://github.com/leejet/stable-diffusion.cpp/tree/77b72a2efed78f55de26892c7e909cee001e8b83/examples/common) | `77b72a2efed78f55de26892c7e909cee001e8b83` | `apps/common/` | 11 个 C++ 源文件与头文件 |
| [sd.cpp examples/cli](https://github.com/leejet/stable-diffusion.cpp/tree/77b72a2efed78f55de26892c7e909cee001e8b83/examples/cli) | 同上 | `apps/cli/` | 3 个 C++ 源文件与头文件 |
| [sd.cpp examples/server](https://github.com/leejet/stable-diffusion.cpp/tree/77b72a2efed78f55de26892c7e909cee001e8b83/examples/server) | 同上 | `apps/server/` | 13 个 C++ 源文件与头文件，加 `recipes_builtin.h.in` |
| [sdcpp-webui](https://github.com/leejet/sdcpp-webui/tree/fa93429c47c924054e6a9c14f8d7b8e06f57a0ec) | `fa93429c47c924054e6a9c14f8d7b8e06f57a0ec` | `frontend/` | 42 个已跟踪文件，含 MIT LICENSE 和 pnpm lockfile |

原生 27 个源文件/头文件、recipe 模板与许可证在本次导入时与来源逐字节一致；应用 CMake
为本仓库新建。原有模型管理、参数解析、媒体 IO、日志、HTTP routes、异步任务和资源释放
均保留。没有迁入模型网络、GGML 或后端私有实现。

## 本地构建差异

- 本地程序使用 `sd-app-cli`、`sd-app-server`，避免与上游入口 target 或输出文件冲突。
- 原生公共代码在 `sd_app_common` 中编译，两个应用链接同一份引擎。
- 使用公开 `stable-diffusion.h` 和上游 thirdparty headers；不加入引擎 `src/` 私有 include 路径。
- shared C API import、Threads 和选中的系统 codecs 在本地 CMake 作用域明确解析。
- 内置模型 recipes 仍由固定 vendor 的目录生成，当前基线包含 108 个 JSON，不复制到应用目录。
- 本地 UI 使用 Vite 开发代理，或通过 `--serve-html-path` 提供构建好的单 HTML；原生构建不隐式安装 Node 依赖。

上游 `SD_BUILD_EXAMPLES=ON` 仍可以同时构建原始入口，作为共存与行为验证路径。
本地 `dev-cpu` 只选择应用入口，避免日常开发重复编译两套程序。

## Web UI 的本地修改

迁入后只调整三个源文件：

- `package.json`：应用包名以及 preview 命令。
- `vite.config.js`：loopback 地址、固定 dev/preview 端口与 `/sdcpp` 开发代理；目标由明确的 `SD_APP_BACKEND_URL` 选择。
- `README.md`：迁移来源与本地开发说明。

UI 的模型选择、能力表单、请求映射和已有 Backend base URL 设置沿用原有实现。
`pnpm-lock.yaml` 与 `LICENSE` 保持来源内容。

## 后续同步

引擎同步与应用修改分别记录。升级 sd.cpp 时复核公开 API、新参数、builtin recipes 和
本地 HTTP 映射；需要吸收上游前端变化时，以上表提交为共同基线逐项比较，再修改应用代码。
不通过覆盖整个应用目录来抹掉本地功能，也不修改 vendor 来配合旧的前端假设。

本轮提供已有 CLI、HTTP 和 UI 的本地开发基础；新的跨节点 JSON 工作流和 CatV2TON 网络
仍需要独立实现与验证。
