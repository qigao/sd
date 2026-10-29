# 架构与实现边界

本仓库是独立应用工程。目标是在完整的 sd.cpp 能力上实现模型管理、HTTP 服务和 JSON
工作流；KFCore 通过可选 SDK/适配器提供需要的图像处理或其他计算能力。

**当前保留可编辑的原生 CLI/HTTP 基线，Node.js/pnpm/Vue/Vite 应用链已删除。**
本地程序为 `sd-app-cli`、`sd-app-server`；当前 server 仍沿用原有 httplib 与生成协议。
服务重构以 CHttp 的 HTTP/2、WebSocket、认证和应用 Service 能力为基础。
[CHttp 服务设计](chttp-service-design.md) 记录固定源码事实、客户端协议和接入前置能力。
新的 CHttp 服务与跨节点工作流尚未实现，不把旧 HTTP 基线作为它们的验收结果。

## 代码与状态归属

| 层 | 归属与职责 | 当前状态 |
| --- | --- | --- |
| sd.cpp / GGML | 上游维护模型架构、权重加载、采样、计算后端、设备与内存放置 | 通过未修改的固定 gitlink 引入 |
| 上游 CLI/server | 使用上游参数、recipe、模型管理与生成 API | 按构建配置启用 |
| 本地原生前端 | 调用公开 C API、参数与结果所有权、媒体 IO | 已迁入 `apps/cli` 与 `apps/common` |
| 应用模型与生成任务 | 模型目录/recipe、加载/卸载、图像/视频任务、进度与取消 | 已迁入 `apps/server`，保持来源语义 |
| 当前 HTTP 基线 | 原生 routes、模型与生成接口 | 已迁入 `apps/server`，尚未替换为 CHttp |
| 目标服务入口 | CHttp HTTP/2 命令与文件传输、WS 事件、可信身份和权限 | 设计与共享层缺口已记录 |
| 跨节点工作流 | 版本化 JSON、节点编排、跨 provider 数据与资源管理 | 待实现 |
| KFCore / ONNX / 其他能力 | 可选的图像处理、检测、分割或其他工作流节点 | 按实际需求接入 |

所有自有服务和流程代码放在本仓库。不要将本应用移回 KFCore，不要复制 sd.cpp 的模型
计算代码，也不要让 ONNX 的图输入输出、设备支持或格式限制成为 sd.cpp 的约束。
源码来源与精确迁移范围见 [前端迁移记录](frontend-migration.md)。

## 保留上游能力

构建层直接包含上游项目。`SD_*` / `GGML_*` 参数、选中的 targets 和安装行为保持上游
语义；CPU presets 只定义当前验证组合。不会增加另一套硬件白名单、强制关闭其他后端，
或把 `SD_CUDA` 绑定到 KFCore 的 CUDA/ONNX 开关。

应用适配层使用 sd.cpp 的
[公开 C API](https://github.com/leejet/stable-diffusion.cpp/blob/77b72a2efed78f55de26892c7e909cee001e8b83/include/stable-diffusion.h)，
依赖默认参数初始化和能力查询。通用任务字段只统一输入、输出和执行状态；上游特有的
加载、设备、内存、采样参数需要保留其语义。同步上游时复核新增字段与 API 变化，不能
静默丢弃未映射的用户选项，也不能声称一次封装会自动支持所有未来接口。

默认 GGML 版本由 sd.cpp 自身记录。上游提供的自定义/system GGML 选项遵循其兼容性
契约，不能沿用默认组合的测试结论。源码、公开 API、构建依赖与回滚流程见
[上游同步](upstream-sync.md)。

## 模型定义与加载

sd.cpp 的
[原生服务协议](https://github.com/leejet/stable-diffusion.cpp/blob/77b72a2efed78f55de26892c7e909cee001e8b83/examples/server/api.md)
已有 recipe、模型文件目录、加载/卸载/状态、生成任务与能力查询。Recipe 已描述组件
slots、默认参数和部分 context 选项。优先利用这些已有语义，避免维护第二份模型家族
名单或推理默认值表。本地 server 已保留这些实现，内置 recipes 直接读取 vendor 的同一
目录。模型根目录的用户 recipes 继续使用原有覆盖与扩展机制。

模型定义需要区分三类信息：

- **模型身份与文件**：固定版本、文件摘要、来源、组件角色和兼容配置。
- **部署配置**：设备、权重放置、内存预算、加载策略及 worker 资源。
- **生成请求**：输入媒体、提示词、采样参数和输出要求。

不同模型的 diffusion、VAE、text encoder 等组件必须按角色显式关联。多个文件存在于同一
目录或具备相同扩展名，并不能证明可以组合加载。上游支持的模型格式直接交由上游处理，
无须先转换成 ONNX。

接入 KFCore SDK 时，优先复用其
[ModelPackage](https://github.com/qigao/KFCore/blob/cf6a7e6e21151bc9f5c8792df324cc06c1a3ad2f/docs/runtime/model-package-v1.md)
的身份、路径边界、摘要校验与语义配置能力。它的一般 artifact 格式并不限于 ONNX。
组件角色仍需显式定义；现有“可选执行 artifact”不能自动解释为一组必须共同加载的权重。
本阶段尚未引入 KFCore SDK，也没有新增一套模型包解析器或 hash 实现。

## Session、任务与 HTTP

本地前端保留已有的 context 创建、模型切换、生成与结果释放实现。后续需要用真实权重
验证完整的加载、生成、交付与释放路径，不能把无模型 HTTP 通过当作该路径已验收。
继续修改时必须维持参数存储、context 与结果的所有权；模型切换、卸载和退出应遵循
已有在途任务约束。

迁入的 server 保持一个已加载 context，任务排队或运行时拒绝模型切换。这是当前原生 worker
的边界。公开 progress/preview callback setter 是进程级接口；并发扩展需要
先明确回调路由、生命周期和取消归属。多个 worker 的调度属于应用层，不要求改写后端。

目标 HTTP/2 层负责认证、校验与接纳请求、返回任务身份、查询状态和交付产物。
WS 推送来自同一任务事实源的版本化事件，断线后显式查询快照；任务执行器拥有排队与
运行状态，容量和资源预算必须明确。HTTP handler 不直接拥有长期 GPU context，不通过
无限排队隐藏资源不足；取消、超时和关闭需要统一的终态与清理路径。

每个任务、输入和结果都绑定可信身份，读取、取消、订阅与下载分别校验权限。HTTP/2
物理连接不能作为身份，borrowed JWT/Session/request 指针不能进入 worker。原有 OpenAI、
SDAPI 等入口若继续提供，必须映射到同一个有界业务队列，不能绕过资源和权限准入。

当前 HTTP API 仍是迁入的上游协议；CHttp 接入、可信调用上下文、新工作流与多 worker
尚未实现。生产推理使用原生程序；测试中的 Python 仅观察服务行为。

## JSON 流程定义：拟定方向

模型定义、部署配置、任务和工作流应分别带版本。工作流描述节点、输入输出连接和显式
参数；执行时引用已登记的模型与部署配置。provider 专属配置保留在自己的命名空间，
公共字段只承诺跨 provider 一致的语义。

首个版本应验证可执行节点、必需输入、依赖关系、资源配置和输出所有权，明确拒绝不支持
的字段或操作。流程定义不会生成缺失的网络实现，也不能把所有不同引擎强行压成一个
ONNX `run(inputs, outputs)` 接口。具体 schema、路由和错误码将在对应实现与行为测试
完成后发布，本阶段不提供可提交执行的示例协议。

## 原生 JSON Workflow V1 状态

V1 已在本地 httplib 基线增加 `POST /sdcpp/v1/workflows`，复用现有
模型 manager、AsyncJobManager 单 worker、sd.cpp 图像生成与取消。
支持严格 version=1 JSON、recipe + 当前服务模型 revision 的入队绑定、
最大四节点的生成 DAG（控制依赖）和有界输出；仅允许 loopback 开发服务。
`depends_on` 不传递像素，暂不支持动态模型加载节点、KFCore、
CatV2TON 或跨节点媒体输入。详见 [工作流 V1](workflow-v1.md)。

这是独立于正式 CHttp HTTP/2 与鉴权服务的过渡入口；无权重的解析/HTTP
门禁不代表任何真实权重或 GPU 计算已经通过。

## CatV2TON 与扩展模型

本仓库没有实现 CatV2TON 的网络或工作流。其
[参考 pipeline](https://github.com/Zheng-Chong/CatV2TON/blob/d8abdab93c9e3ffc89f6f9e13dbbee6b88e85cfc/modules/pipeline.py)
包含特定 VAE、DiT、可选 PoseNet、调度和视频处理语义。添加 recipe 只能组合引擎已有的
能力；若缺少相应网络或算子，需要向上游贡献支持，或由 vendor 之外的独立原生 provider
提供，并完成组件与真实模型验证。不会为此维护私有 sd.cpp 分支。

KFCore Draft PR #356 中的私有 conditioning 探索保留在原处，不代表 CatV2TON 已可运行。
Sol 集成仍单独评估，不阻塞现有引擎的服务接入。

## 后续实现顺序

1. 去除 Node 应用链，以 CHttp 的固定源码明确客户端认证、任务和产物契约。
2. 补齐共享 Service 的结构化输入输出与可信 invocation context，验证 HTTP/2 身份隔离；
   不在 sd 另造反射、参数绑定或 RPC 框架来绕过这些能力缺口。
3. 提取唯一任务运行时，修复已识别的状态快照、异常释放、回调生命周期与资源上限，
   接入 HTTP/2 的提交、查询、取消、上传和结果交付。
4. 接入有界 WS 事件订阅；WS RPC 若启用，先在共享层补 dispatcher 和每消息授权上下文。
5. 使用真实权重验证模型加载、生成和结果生命周期，再扩展 JSON 工作流、KFCore 节点与多 worker。

构建成功、无权重服务响应、真实模型正确性、硬件兼容和性能分别记录。一个验证层的通过
不能代替其他层的证据，也不作为裁剪上游功能的理由。
