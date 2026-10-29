# 原生前端源码迁移记录

本仓库维护原生入口，模型计算和 GGML 保留在未修改的 sd.cpp 子模块中。

| 来源 | 固定提交 | 本地目录 | 导入范围 |
| --- | --- | --- | --- |
| [sd.cpp examples/common](https://github.com/leejet/stable-diffusion.cpp/tree/77b72a2efed78f55de26892c7e909cee001e8b83/examples/common) | `77b72a2efed78f55de26892c7e909cee001e8b83` | `apps/common/` | 11 个 C++ 源文件与头文件 |
| [sd.cpp examples/cli](https://github.com/leejet/stable-diffusion.cpp/tree/77b72a2efed78f55de26892c7e909cee001e8b83/examples/cli) | 同上 | `apps/cli/` | 3 个 C++ 源文件与头文件 |
| [sd.cpp examples/server](https://github.com/leejet/stable-diffusion.cpp/tree/77b72a2efed78f55de26892c7e909cee001e8b83/examples/server) | 同上 | `apps/server/` | 13 个 C++ 源文件与头文件，加 recipe header 模板 |

27 个原生源文件/头文件、recipe 模板与许可证在初次导入提交
[`84bee018`](https://github.com/qigao/sd/commit/84bee0182e6e5ea5c820b6c2c0eab334dd8f20b0)
时与来源逐字节一致；应用 CMake 是本仓库的接入代码。没有复制模型网络或 GGML 实现。

## 当前本地差异

- 程序命名为 `sd-app-cli`、`sd-app-server`，可与上游入口共存。
- 原生公共代码通过 `sd_app_common` 编译；使用公开 C API 和上游第三方 headers。
- shared import、Threads 和系统 codecs 在应用 CMake 作用域明确接入。
- 内置模型 recipes 直接从 vendor 目录生成，不复制第二套 catalog。
- 已从 owned server 删除引用生成 Web UI header 的条件编译分支。
- 保留原有外部 HTML 文件服务行为，它不构成 Node 依赖，也不是新的客户端产品。

## Node 应用链删除

初次导入同时包含的 `frontend/` 已按用户决定完整删除，包括 42 个文件、许可证副本、
包清单、锁文件、构建脚本、Vue/TypeScript 源码，以及本地生成产物。对应前端 CI 和启动
说明一并移除。历史来源留在上述 Git 提交中；当前应用不再引用该目录。

vendor 自身保持上游原样，其可选 examples/Web UI 属于上游源码，未因本应用重构而修改。
本仓库的 native presets 不执行上游前端构建。

## 服务重构

服务使用 CHttp 的协议、认证和应用能力，目标为 HTTP/2 + WS。当前迁入的 HTTP 代码是
推理/行为基线，尚未替换为 CHttp；具体事实、缺口和接入顺序见
[CHttp 服务设计](chttp-service-design.md)。

同步引擎时比较公开 API、参数与 recipes，修改应用适配，按 [上游同步](upstream-sync.md)
验证。不要覆盖应用目录来抹掉本地改动，也不要修改 vendor 迁就旧适配。
