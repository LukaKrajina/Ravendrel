# Ravendrel Web UI

QKM 本地推理的浏览器界面（Vite + TypeScript + Tailwind CSS + Dexie）。

## 功能

- 多轮对话（本地 IndexedDB 持久化历史）
- SSE 流式推理输出，支持中途停止
- 暗色 / 亮色主题、5 语言 i18n、移动端抽屉
- PWA 支持（可安装）
- 动态获取后端模型列表

## 运行

```bash
npm install
npm run dev        # 开发
npm run build      # 构建（含 PWA）
npm run preview    # 预览构建产物
npm test           # 单元测试（Vitest）
```

## 后端约定

后端地址默认 `http://localhost:9080`，可用环境变量 `VITE_RVDL_API_BASE` 覆盖。

- 推理：`POST {base}/v1/chat/completions`
    - 请求体：`{ model, messages: [{ role, content }], stream: true }`
    - 流式响应：`text/event-stream`，`data: {...}` 分块，`data: [DONE]` 结束
- 模型列表：`GET {base}/v1/models`，返回 `{ data: [{ id }] }`

## 目录结构

- `src/main.ts` 应用入口与交互逻辑
- `src/db.ts` Dexie 本地数据库（chats / messages）
- `src/i18n.ts` 多语言支持
