# sd Web 前端

本目录是可独立编辑的应用前端，基于
[leejet/sdcpp-webui](https://github.com/leejet/sdcpp-webui/tree/fa93429c47c924054e6a9c14f8d7b8e06f57a0ec)
提交 `fa93429c47c924054e6a9c14f8d7b8e06f57a0ec` 迁入。原有 Vue 页面、原生 API
调用、组件选择和图像/视频表单保留；来源的 [MIT LICENSE](LICENSE) 与 lockfile 保留。
开发时修改这里的源码，不修改 `vendor/stable-diffusion.cpp` 内的前端子模块。

本地调整包括包名、开发代理和固定端口。依赖仍采用来源版本的 `pnpm-lock.yaml`。
应用使用既有 `/sdcpp/v1/*` 协议；目前没有新增 JSON 工作流协议。

## 开发

需要 Node.js 20 或更新版本、pnpm 10.15.1，以及运行中的原生 `sd-app-server`。
完整的后端构建和跨平台命令见 [本地开发](../docs/local-development.md)。

```sh
pnpm install --frozen-lockfile
pnpm dev
```

开发地址固定为 `http://127.0.0.1:5173`。在 UI Settings 中将 Backend base URL 留空，
`/sdcpp` 请求就会通过代理转到 `http://127.0.0.1:1234`。启动前设置
`SD_APP_BACKEND_URL` 可更改代理目标；修改这个变量需要重新启动 Vite。
如在 UI 中指定了非空 base URL，请求会直接使用该地址。

端口被占用时启动失败，不自动换端口。代理连接失败也会直接报告错误，不启动或切换模型服务。

## 验证与产物

```sh
pnpm type-check
pnpm build
pnpm preview
```

构建产物是单个 `dist/index.html`。Preview 地址固定为 `http://127.0.0.1:4173`，
使用相同的 `/sdcpp` 代理配置。原生服务可通过 `--serve-html-path` 直接提供该 HTML；
不需要在服务中运行 Node.js。`build:header` 保留来源的可选嵌入头文件生成能力，
本地开发默认使用外部 HTML。
