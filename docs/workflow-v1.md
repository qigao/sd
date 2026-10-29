# JSON Workflow V1（原生单进程基线）

本规范在 sd-app-server 的既有 httplib /sdcpp 路由上实现。当前还没有接入
CHttp HTTP/2、WebSocket、可信身份、产物存储或多租户权限；**不是**
docs/chttp-service-design.md 的未来 /v1/jobs API。只允许绑定 127.0.0.1
或 ::1 的本地开发服务提交 V1。临时入口会拒绝带 Origin/Referer 的请求及非本地 Host，
同一约束适用于 Workflow job 查询、结果和取消；但这仍然不是调用者认证，其他旧路由
仍保留原来的安全边界。不能因为本功能存在就开放现有 httplib 服务到公网。

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
  "model": {"recipe": "@command-line", "revision": 0,\n            "instance": "0123456789abcdef0123456789abcdef"},
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
version 严格为整数 1；model.recipe、model.revision、model.instance 必填，instance 是 1–64 位小写十六进制串。
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
- 成功结果中被选择的 PNG base64 与 generation metadata 合计最高 8 MiB，
  超出时显式失败并不发布部分结果。现阶段通过**原有** job status
  JSON 返回这段有界媒体，尚不是 CHttp artifact/streaming endpoint。
- 工作流在本进程内保存，沿用原任务的 TTL；不保证重启恢复或持久幂等。
  工作流 JSON 中不保存 SDK context、原生指针、HTTP request/WS 连接。
- 请求体限制当前是在 httplib 已接收 body 后检查；它**不是**上传层流式准入，
  更不能替代将来 CHttp 的 streaming input/memory quota。

服务关闭时停止新准入、取消排队中的任务并请求中断运行中任务，worker join 后再释放 context。
取消运行节点时现有 sd.cpp monitor 请求取消，但仍需等待其真实计算和结果
RAII 清理才写 cancelled；中断不强行释放仍被 native 代码借用的 context。
节点异常形成 failed，不跳过关闭保护。没有模型时合法提案返回 503；
不匹配的 recipe/revision/instance 返回 409；不支持 generation mode 或继承了 V1 不允许的默认参数返回 422。

## 验证层次

| 验证 | 证据定义 | 不可推论 |
| --- | --- | --- |
| 原生解析/调度 CTest | 严格 JSON 拒绝、拓扑顺序、循环、注入失败、取消和异常终态 | GPU / 模型输出 |
| 无权重真实 HTTP CTest | 启动 sd-app-server，检查 400/413/503 以及普通 catalog/status | 真实生成 |
| 真实模型资格门禁（可选，尚无运行证据） | 使用本地固定 SHA-256 manifest 的 /models/load、submit→poll→cancel/result、PNG CRC、切换和 reload 竞态 | 未提供权重时不得宣称通过 |
| CHttp/H2/WS/鉴权（未实现） | 可信调用身份、按任务授权、流式 artifact、状态事件/背压及关闭竞态 | 由 httplib 测试替代 |

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
