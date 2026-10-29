const { defineConfig } = require("vite");
const vue = require("@vitejs/plugin-vue");
const { viteSingleFile } = require("vite-plugin-singlefile");

function backendTarget() {
    let target;
    try {
        target = new URL(process.env.SD_APP_BACKEND_URL ?? "http://127.0.0.1:1234");
    } catch {
        throw new Error("SD_APP_BACKEND_URL must be an absolute HTTP(S) URL.");
    }
    if (!["http:", "https:"].includes(target.protocol) ||
        target.username || target.password || target.search || target.hash) {
        throw new Error("SD_APP_BACKEND_URL must use HTTP(S) without credentials, query or fragment.");
    }
    return target.href;
}

const proxy = {
    "/sdcpp": {
        target: backendTarget(),
        changeOrigin: true,
    },
};

module.exports = defineConfig({
    plugins: [
        vue(),
        viteSingleFile(),
    ],
    server: {
        host: "127.0.0.1",
        port: 5173,
        strictPort: true,
        proxy,
    },
    preview: {
        host: "127.0.0.1",
        port: 4173,
        strictPort: true,
        proxy,
    },
});
