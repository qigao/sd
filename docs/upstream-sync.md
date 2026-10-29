# 同步 sd.cpp 上游

sd.cpp 和它的子模块保持未修改。`vendor/stable-diffusion.cpp` 的 gitlink 是本仓库选择的版本；不在构建脚本中再写一份固定 SHA，也不在后台自动跟随 `master`。

## 更新流程

1. 选择要验证的上游 commit，阅读其公开 API、构建选项、模型及 recipe 变更。
2. 在独立开发分支更新子模块，并更新该版本所需的嵌套依赖。以下命令中的 `UPSTREAM_COMMIT` 必须替换为实际、已选定的 commit。

   ```sh
   git -C vendor/stable-diffusion.cpp fetch origin
   git -C vendor/stable-diffusion.cpp checkout --detach UPSTREAM_COMMIT
   git -C vendor/stable-diffusion.cpp submodule sync --recursive
   git -C vendor/stable-diffusion.cpp submodule update --init ggml
   git add vendor/stable-diffusion.cpp
   ```

3. 以新 build tree 或明确重置过的缓存重新配置。gitlink 已暂存后即可验证，无须先提交；只运行增量 build 不会因为 index 变化而重新校验版本。

   ```sh
   cmake --preset cpu-release
   cmake --build --preset cpu-release
   ctest --preset cpu-release

   cmake --preset cpu-shared-server
   cmake --build --preset cpu-shared-server
   ctest --preset cpu-shared-server
   ```

4. 核对公共 C API、结果释放、设备枚举、原生 CLI/server 入口和已覆盖 HTTP 行为。修改本仓库的前端适配和测试以跟随明确的上游变更，不修改 vendor 来满足旧测试。
5. 对实际使用的硬件、模型和配置运行相应资格验证。CPU 无权重测试只验证接入，不证明采样结果、GPU 支持、RPC 远程运行、打包或性能。
6. 更新 README/第三方来源中的版本链接、适配说明及验证证据，把 gitlink 与必要的前端改动放在同一个可审查提交中。

CPU 测试只需要 sd.cpp、其 GGML 子模块和测试用 Salts。CUDA、媒体编码或 Web UI 可能还需要嵌套子模块、SDK 或前端工具，按选定 commit 的 [上游构建文档](https://github.com/leejet/stable-diffusion.cpp/blob/77b72a2efed78f55de26892c7e909cee001e8b83/docs/build.md) 初始化。不要把 CPU CI 的依赖集合当作所有部署的完整依赖。

## 需要核对的边界

- **功能完整性**：不维护与上游分叉的后端/模型白名单，不把前端尚未呈现的能力在构建层关闭。使用上游选项及公开能力接口。
- **参数与模型**：recipe 负责组合引擎已支持的组件。模型架构新增或缺失算子需要上游支持或独立 provider；改一份 JSON 无法实现它们。
- **依赖身份**：默认 GGML 跟随 sd.cpp 的记录。显式 system/custom GGML 由上游配置选择，需独立验证其头文件和二进制兼容性。
- **状态与并发**：现有 sd.cpp progress/preview 设置接口是进程级。模型切换、在途任务和回调的生命周期由前端设计明确；同步不能改变这些约束而不更新适配。
- **部署**：保留上游默认构建图和 install rules。测试 profile 可以选择静态、共享、动态 backend 或服务组合，不强制所有部署使用同一组合。

## 回滚

通过新的回滚提交恢复之前的 gitlink 和对应前端适配，不重写共享分支历史。执行 `git submodule update --init` 恢复工作树，再重新配置、构建和测试。测试用新安装前缀与现有部署分开：关闭选项或回退源码不会删除之前已经安装的库与可执行文件。
