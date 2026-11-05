# SE — 轻量级 rvdl 语言编辑器

rvdl 语言桌面编辑器，复用 server 的编译管线（Lexer → Parser → Semantic → IR）。

## 运行

1. 先启动守护进程：`runtime --daemon`
2. 安装依赖并启动编辑器：

```bash
npm install
npm start
```

## 结构

- `src/main.ts` Electron 主进程
- `src/pipeline.ts` 复用 server 编译管线
- `src/runner.ts` 连接 daemon 执行
- `renderer/app.ts` CodeMirror 编辑器 + 输出 Shell

## 技术栈

- Electron（桌面窗口）
- CodeMirror 6（编辑器，复用 JS 语法高亮）
- 复用 `server/src` 的 lexer / parser / semantic / ir
