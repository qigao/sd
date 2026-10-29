# sd

独立的原生图像与视频生成前端、HTTP 服务和工作流开发仓库。

- 推理引擎使用未修改的 [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp)，保留上游功能并按固定版本同步。
- 本仓库负责模型管理、服务入口、任务与流程定义，以及输入输出处理。
- KFCore、ONNX Runtime 等能力按需接入；它们不限制 sd.cpp 的模型、硬件或部署方式。

构建与验证基础通过后续开发分支提交审查。
