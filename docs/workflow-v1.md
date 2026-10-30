# JSON Workflow V1（原生单进程基线）

本规范在 sd-app-server 的既有 httplib /sdcpp 路由上实现。当前还没有接入
CHttp HTTP/2、WebSocket、可信身份、产物存储或多租户权限；**不是**
docs/chttp-service-design.md 的未来 /v1/jobs API。只允许绑定 127.0.0.1
或 ::1 的本地开发服务提交 V1。临时工作流入口继续拒绝带 Origin/Referer 的请求及非本地 Host，
同一约束适用于 Workflow job 查询、结果和取消。自有 httplib 服务也不再反射任意
Origin/启用凭据式 CORS；默认 loopback 绑定拒绝非本地 Host，所有自有路由拒绝跨源
Origin/Referer。合法同源请求不需要 CORS 授权头。但它仍然不是用户认证，
非 loopback 的旧服务也不是受保护的多租户 API。不能因为本功能存在就开放现有 httplib 服务到公网。

## 模型身份与既有引擎

模型仍由原来的 /sdcpp/v1/recipes、/sdcpp/v1/models/load、
/sdcpp/v1/models/unload 和 /sdcpp/v1/models/status 管理。Workflow
不拥有第二套 recipe、模型解析或推理 provider。先加载 compatible 模型，
再从 status 读取当前 recipe、revision、instance。基于命令行权重启动的 ready 状态使用
recipe="@command-line" 和 revision=0（必须有 origin="command_line"），但仍需绑定本次启动的 instance。
普通 load 使用 status.recipe、status.revision 的精确值。入队时在模型共享锁下绑定
三者；模型切换因 pending job 而拒绝。再次执行前验证绑定，运行中的模型只由
现有模型 manager/context owner 保持，不持久化原生指针。

instance 是每次服务进程启动产生的非授权 nonce，防止跨进程沿用相同 revision 的旧工作流，不是安全凭据。
`AsyncJobManager` 也复用这一实例身份，所有新 job ID 形如
`job_<instance>_<timestamp>_<ordinal>`；重启后即使计数器与时间碰撞，
也不会误将新进程任务标识等同于旧任务。轮询旧 ID 会得到未知任务，
客户端不能自动重放工作流。实例 ID / job ID 都**不是权限凭据**。
revision 是**当前服务实例的模型变更序号**，不是文件摘要、跨重启版本、
可复现权重身份或持久修订。完整的来源/组件摘要身份留待 ModelPackage 等后续功能。
不支持在 Workflow 中动态加载或卸载模型；这会打破当前单 context 与 pending
任务的隔离。模型准备不是一个伪装的执行节点。

## V1 合同

提交 POST /sdcpp/v1/workflows，Content-Type: application/json，
收到 202 + 原有 job id/poll_url；使用
GET /sdcpp/v1/jobs/{id} 查询，并通过
POST /sdcpp/v1/jobs/{id}/cancel 取消。

示例中的 instance、revision 必须替换为实际 /models/status 返回值；无权重状态不会接纳：

```json
{
  "version": 1,
  "model": {"recipe": "@command-line", "revision": 0,
            "instance": "0123456789abcdef0123456789abcdef"},
  "nodes": [
    {
      "id": "draft",
      "type": "sdcpp.image.generate",
      "depends_on": [],
      "params": {"prompt": "a mountain lake", "negative_prompt": "",
                 "width": 256, "height": 256, "steps": 12,
                 "seed": 42, "cfg_scale": 7.0}
    },
    {
      "id": "alternate",
      "type": "sdcpp.image.generate",
      "depends_on": ["draft"],
      "params": {"prompt": "a different mountain lake",
                 "width": 256, "height": 256, "steps": 12}
    }
  ],
  "outputs": ["draft", "alternate"]
}
```

所有 JSON object 的字段集合是封闭的，未知字段、错误类型、缺少必需字段、
重复节点/依赖/输出、缺失依赖、环以及不支持的 node type 均返回 400。
version 严格为整数 1；model.recipe、model.revision、model.instance 必填，instance 必须是本次服务返回的 32 位小写十六进制值。
节点 ID 为 1–48 字节 ASCII 标识符，首字符必须是英文字母。

V1 只支持 **sdcpp.image.generate**：单张 PNG text-to-image。每节点
prompt、width、height、steps 必填；negative_prompt、seed、cfg_scale 可选。
prompt 最长 2048 字节；尺寸 64–1024、8 的整数倍；steps 1–100；
seed 为 -1..INT64_MAX；cfg_scale 必须为有限数、范围 0..30。
采样器、context/recipe 默认参数沿用已加载模型的现有 sd.cpp 配置。
V1 拒绝要求多图 batch、hires 和显式图像输入的 inherited defaults，
不隐式开启 ADetailer。当前不接受文件路径、任意网络上传或客户端指定输出目录。
上游 SD_* / GGML_* 的其它功能**没有被删除**；只是不属于这个 V1 JSON 子集。

depends_on **只表达控制依赖（完成后才执行）**，不传递上游图像像素，也不隐含
img2img、变量插值或动态参数。解析器将 DAG 拓扑排序；所有节点都真实调用现有
execute_img_gen_job()/generate_image()，不提供空计算节点。outputs 选择
哪些节点的输出在成功时保留；未选中节点的生成图在该节点完成后释放。
入队前还会按当前已加载模型的默认参数预验证每个节点；只有通过预验证才发布任务。
任何节点失败或取消，后续节点不运行，部分结果不发布。
KFCore、ONNX、CatV2TON、视频节点与跨节点媒体边在 V1 均是显式 unsupported，
不得将 schema 合法性误当成网络已实现。

## 所有权、取消与限额

Workflow 复用 AsyncJobManager 的同一个单 worker、队列、状态读取、queued
取消与 sd_cancel_generation()。节点生成参数和采样指针在整个 native 调用期间
由任务 owner 保持；引擎返回的图像由现有 SDImageVec 释放。每节点状态只有
manager.mutex 下的读写；终态发布后不再执行下一节点。

- 请求 JSON 最高 64 KiB；1–4 个节点；单 DAG 所有节点合计像素至多 2 MiPx。
- 最多保留 4 个未过期 Workflow job（包含成功/失败/取消的 TTL 项），并参与
  现有 64 个 pending 全局额度；超出返回 429。
- 所有任务类型的 **retained registry 总量上限为 256 项**（包含 queued、running、
  completed、failed 和 cancelled），与 64 个 pending 配额独立。达到上限后
  新提交统一返回 429：原生 image/video 响应 `job retention capacity is full`，
  Workflow 响应 `retention_capacity`。到期清理先于容量检查；**不会为接纳新任务
  提前移除任何未过期结果**。该记录数上限本身不能当作完整内存限额。
- 完成态的**编码媒体 base64** 增加独立上限：每任务最多 **128 MiB**，
  当前进程所有 *registry-owned* 完成任务媒体总计最多 **256 MiB**。
  生产 worker 在 manager 锁下原子计算并登记已选中图片、视频和 Workflow
  输出中的 base64 字节，不序列化整个 JSON。超出任务上限形成
  `result_too_large`，总额不足形成 `result_capacity` 的 Failed 终态；
  不发布任何部分媒体，不预先删除已有结果。正常完成后，配额占用一直保留至
  该任务 TTL 到期并从 registry 中删除，取消/失败不占媒体配额。
  HTTP reader 持有的 `shared_ptr` lease 可以在 TTL 过期后安全完成，
  **这些短期借用副本不计入 registry 指标**。
  当前预算只覆盖保留的编码媒体，不包括元数据、临时编码缓存、输入/上传、
  GPU 上下文、网络发送缓冲和持久 artifact；它不是完整内存配额。
  完整输入/输出内存字节、磁盘和用户身份配额仍由正式 CHttp 实现。
- 成功结果中被选择的 PNG base64 与 generation metadata 合计最高 8 MiB，
  超出时显式失败并不发布部分结果。现阶段通过**原有** job status
  JSON 返回这段有界媒体，尚不是 CHttp artifact/streaming endpoint。
- 任务达到 Completed/Failed/Cancelled 或被排队取消时，生产终态 owner 释放
图像/视频的输入像素、音频、引用媒体、LoRA/缓存参数和临时指针视图，不再通过
重新构造完整 `SDGenerationParams` 在 `manager.mutex` 内分配内存。
Workflow V1 同时退休原始 prompt/negative_prompt、节点参数、依赖边与拓扑数组，
但保留节点 ID、终态状态和已经发布的结果 JSON，保证查询合同不变。
没有执行中的 sd.cpp 调用会在这些资源退休后继续借用它们。
原生测试直接检查三类任务在成功、失败和排队取消后不再保存输入与 DAG。

- 终态成功/失败/取消后的字段不可变。HTTP 查询/取消先在 manager 锁内取得
  `shared_ptr` reader lease，再在锁外构建终态 JSON；TTL 可从 registry 清除该任务，
  但在途 reader 的媒体不会被提前释放。排队/运行状态仍须加锁读取。
- 工作流在本进程内保存，沿用原任务的 TTL；不保证重启恢复或持久幂等。
  工作流 JSON 中不保存 SDK context、原生指针、HTTP request/WS 连接。
- 请求体限制当前是在 httplib 已接收 body 后检查；它**不是**上传层流式准入，
  更不能替代将来 CHttp 的 streaming input/memory quota。

服务关闭时停止新准入、取消排队中的任务并请求中断运行中任务，worker join 后再释放 context。
原生异步 worker 和旧 SDAPI 同步预览回调现在使用同一个
`sd_app_try_encode_preview()` 边界：至多 4 MiPixels、8 MiB PNG/JPEG
编码字节及对应有界 base64，且像素/编码/转换失败不更改已发布的预览。
编码器/分配器异常均在 sd.cpp C ABI 回调内部截获，不可跨越 native ABI；
这只限制可选预览，不截断正常推理。原生 CTest 使用实际 PNG/JPEG
编码器及注入的编码/序列化故障验证边界，真实模型 callback 仍需 Issue #2。
旧 SDAPI/OpenAI 同步 `generate_image()` 现在尊重 C API 的 bool 返回值：
即使失败时残留部分原生图像，所有权仍由 RAII 清理，不能发布 HTTP 成功。

取消运行节点时现有 sd.cpp monitor 请求取消，但仍需等待其真实计算和结果
RAII 清理才写 cancelled；中断不强行释放仍被 native 代码借用的 context。
节点异常形成 failed，不跳过关闭保护。没有模型时合法提案返回 503；
不匹配的 recipe/revision/instance 返回 409；不支持 generation mode 或继承了 V1 不允许的默认参数返回 422。

## 统一原子接纳与不可变回执

`/sdcpp/v1/img_gen`、`/sdcpp/v1/vid_gen` 与
`/sdcpp/v1/workflows` 现在共享 `admit_async_job()`，
在 `AsyncJobManager.mutex` 下统一检查 shutdown、pending 容量与 Workflow
保留数量。提交前构造一次不可变 202 回执，map 登记与 deque 入队作为一个原子
提交点；入队分配失败将删除未发布的 registry 项。worker 只会在完整提交后
被唤醒，HTTP 不再在通知 worker 后读取可变 `job.status`。

原生并发测试包含 12 路争夺单个 pending 配额、全量队列/关闭拒绝、
保留四个 Workflow 的上限、终态 TTL 腾出空间及重复接纳拒绝。
这些测试证明**本进程中接纳和所有权的一致性**；由于尚未实现正式 CHttp
的可信身份与 `client_request_id` 幂等索引，网络响应丢失之后的
exactly-once/跨重启恢复依然不能保证。不得自动重发推理请求。

## 原生媒体结果接管的异常安全

`SDImageVec` 接管 sd.cpp 图像/视频结果时先固定原生 owner，
预留图像和元数据容量，再逐项转移像素；转换过程中抛出异常，
已转移的像素由 `SDImageVec` 清理，剩余项由原生 C result
释放。接管 C `sd_image_t*` 数组使用独立 RAII 守卫，
不因 `vector::reserve()`/插入失败泄漏尚未接管的像素。
`SDImageOwner` 在确认容器插入成功后才移交所有权，
复制像素时使用溢出检查，拒绝畸形尺寸。

原生 ADetailer 结果在元数据融合前立刻进入 C API 结果 guard，
避免融合异常导致后续缓冲泄漏。C API 的 `generation_info` 可为空指针，
现在统一映射为空字符串，而不是触发 `std::string(nullptr)` 未定义行为。
无权重 `test_sd_app_native_image_owners` 使用真实 sd.cpp C 数据结构和
内存释放函数验证 image/video 结果、C 数组、复制、owner 移交与多次 clear；
还会用非法元数据触发解析异常，证明已转移/未转移像素在异常退出时的归属。
这不代替真实采样/ADetailer 执行、异常注入、GPU 释放与计数监测：
仍需 Issue #2 的原生后端资格验收。

## Native Job Owner 终态验收

生产队列、状态发布、取消/成功的原子仲裁、过期清理与 shutdown 取消
已抽离为 `sd_app_async_state`，由同一个生产 worker 和 CTest 使用。

生产终态提交点现在对三类成功载荷做**结构完整性门禁**：图片必须拥有
数量一一对应的编码媒体与元数据；视频必须提供非空媒体/MIME、正数
fps/帧数和元数据；Workflow 必须包含 V1 标识、模型对象、与声明输出
节点对应的单图媒体，且所有节点先到达 completed 状态。
缺少任何字段、输出个数不匹配或含空媒体时，不发布 `completed`，
而以 `invalid_generation_result` 失败终态明确回收部分结果。
这防止客户端查询完成结果时才因 `generation_info.at(i)` 越界而报 500。
取消与成功竞争仍以 manager.mutex 下先提交的结果为准，取消优先清理媒体。
新增 CTest 直接注入不完整图像/视频/Workflow 成功回执，不创建第二推理 Provider。
`test_sd_app_async_state` 向**生产终态提交点**注入成功、失败与并发取消结果，
验证终态只可提交一次、部分媒体不发布、预览与输入资源退休，以及 tombstone TTL。
这不是第二套模型计算代码、不是模拟生成 Provider，也不等于真实后端错误注入。
真实模型权重执行时的计算失败、异常、显存故障与资源清理仍是 Issue #2 的独立门禁。

## 验证层次

| 验证 | 证据定义 | 不可推论 |
| --- | --- | --- |
| 原生解析/调度 CTest | 严格 JSON 拒绝、拓扑顺序、循环、注入失败、取消和异常终态 | GPU / 模型输出 |
| 无权重真实 HTTP CTest | 启动 sd-app-server，检查 400/413/503 以及普通 catalog/status | 真实生成 |
| 真实模型资格门禁（可选，尚无运行证据） | 使用本地固定 SHA-256 manifest 的 /models/load、submit→poll→cancel/result、PNG CRC、切换和 reload 竞态 | 未提供权重时不得宣称通过 |
| CHttp/H2/WS/鉴权（未实现） | 可信调用身份、按任务授权、流式 artifact、状态事件/背压及关闭竞态 | 由 httplib 测试替代 |

真实权重脚本在调用 /models/load 之前，从 /recipes 与 /model-files 解析显式
模型槽、recipe 隐式默认组件与 tae/taesd 同义槽，要求**所有最终要加载的组件**
已经具有本地 SHA-256；加载成功后还核对服务回显的完整 model selection。
默认 CTest self-test 覆盖未固定的隐式模型、TAE 默认组件拒绝及非法显式选择。
PNG 验收不只核对容器签名、IHDR 尺寸和 CRC：在读取已完成任务的 PNG
base64 后，以**有上限的 zlib 解压**还原 8 位非隔行灰度/RGB/RGBA
scanline，检查每行 PNG 滤波方式和像素数据长度，记录解码后像素 SHA-256。
测试主动构造 CRC 正确但滤波字节无效、压缩内容损坏、行数据截断和
解压膨胀超额等反例，全部必须拒绝。不支持的 PNG 布局明确失败，
不能把仅 CRC 通过视为真实像素可解码。
这是真实文件摘要的事前准入检查，不是原生推理已成功的证据。
模型目录应在资格验收期间保持只读：哈希完成与原生打开权重之间仍可能存在
外部替换竞争；当前脚本无法提供文件系统事务快照或跨重启 weight revision。

CI 不下载权重。已提供 `tests/workflow_real_model.py` 作为可选的真实权重准入测试，
并由默认无权重 CTest 运行其 manifest/PNG 检查器 self-test（不是模型计算）。
配置 `-DSD_APP_WORKFLOW_TEST_MANIFEST=/absolute/path/to/manifest.json` 时，
CTest 才添加 `test_sd_app_workflow_real_weights`，缺少 manifest 或 digest 不匹配即失败，
不会以 skip 冒充成功。真实测试必须使用可在现有 recipe/catalog 加载的权重；
manifest 需包含 model_root、sha256、load、generation，可选 cancel_steps，
详情见 [本地样例](../tests/workflow-real-model-manifest.example.json)。
使用 `SD_APP_WORKFLOW_QUALIFICATION_TIMEOUT` 可调整每次加载和任务完成等待秒数
（默认 600，范围 60–3600）；pytest/Node/Python 都不是推理 Provider。
该测试覆盖真实图像、排队及运行中取消、任务占用时卸载拒绝、重载后的旧绑定拒绝；
对需要特定故障注入才可复现的计算失败回收仍需单独资格门禁。

CI 不下载权重。今后真实模型资格测试需独立记录完整权重 identity、
精确 HEAD、设备、运行结果和输出，而非从 V1 结构测试推断。

后续扩展需要引入**显式 version/type、具名端口及拥有型媒体引用**；保持 sd.cpp
模型计算权属，KFCore 只作为真实存在、独立验收的可选 node backend。

上述 JSON 示例由原生 CTest 直接解析并经 Workflow V1 解析器验证，不能只检查 Markdown 格式。
