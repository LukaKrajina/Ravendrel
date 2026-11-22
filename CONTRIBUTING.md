# 贡献指南 · Contributing

感谢你为 Ravendrel 量子编程语言做出贡献！本项目由 **OpenMasons** 维护，以 [Apache License 2.0](LICENSE) 开源。

> 提交 Pull Request 即表示你同意：你的贡献按 Apache-2.0 授权，且你有权就相应贡献授予必要专利许可（见 `LICENSE` 第 3、5 节）。

## 仓库一览

| 路径 | 职责 |
| --- | --- |
| `client/` / `server/` | VSCode 扩展客户端与语言服务器（TypeScript）：词法、语法、语义、IR、CLI、HTTP API |
| `runtime/` | C++20 运行时：LLVM ORC JIT、QHAL、QLM、VedaROS、QbNS、QCOS |
| `RSP/` | rvdlRSP 量子机器人仿真平台（物理内核 / Vulkan / Qt GUI） |
| `SE/` | 轻量桌面编辑器（Electron + CodeMirror） |
| `chatWebUI/` | Web 推理聊天界面（Vite + Tailwind + Dexie） |
| `installer/` | Go 编写的 `ravendrelup` 安装器 / 版本代理 |
| `scripts/` | 构建、签名、WDAC、打包脚本 |
| `docs/` | 语言手册与各子系统理论文档 |
| `examples/` | `.rvdl` 示例程序 |
| `vendor/` | 随仓库分发的第三方预编译库（GLFW / Nuklear / stb / pinyin） |

完整目录说明见 [README.md](README.md#-目录结构)。

## 开发环境

- Node.js + TypeScript（VSCode 扩展 / 语言服务器）
- CMake + Ninja + C++20 编译器（运行时）
- LLVM、Kokkos（必需）；Vulkan / GLFW / Qt 6 / CUDA 视目标而定

> ⚠️ 构建前请先阅读 [构建前必读](README.md#️-构建前必读)：仓库中 `.vscode/*.json`、`runtime/.clangd`、`runtime/CMakeLists.txt` 含作者本机绝对路径，需按你的环境修改或用 `-D` 参数覆盖。

## 构建与测试

```bash
# TypeScript 扩展
npm install
npm run compile
npm test

# C++ 运行时
cmake -S runtime -B runtime/build -G Ninja \
  -DLLVM_DIR="<你的 LLVM cmake 目录>" \
  -DKokkos_DIR="<你的 Kokkos 安装>/lib/cmake/Kokkos"
cmake --build runtime/build --parallel

# 运行时测试
ctest --test-dir runtime/build --output-on-failure
```

提交前请确认 `npm run compile` 与相关 `ctest` 通过。

## 代码风格

- C++：遵循根目录 `.clang-format`
- TypeScript：遵循各子项目的 `eslint` / `prettier` 配置
- 提交信息：参考 `.gitmessage` 模板（类型前缀 + 简短描述）

## 提交流程

1. Fork 仓库并创建特性分支（如 `feat/qlm-sparse-backend`）。
2. 一个提交只做一件事；提交信息说明动机而不只是改动内容。
3. 推送后在 GitHub 发起 Pull Request，按模板填写改动说明与验证方式。
4. CI 通过后等待维护者评审；评审意见请在同一 PR 内回复与修正。

## 不要提交

- 构建产物与依赖：`build/`、`node_modules/`、`.deps/`、`bin/`、`dist/`（已在 `.gitignore` 中）
- 私钥与本机私有产物：`*.pfx`、`release.keystore`、`scripts/build/wdac/` 下的部署日志与重建哈希
- 个人 IDE 工作区文件：`*.code-workspace`

详见 [.gitignore](.gitignore) 与 [SECURITY.md](SECURITY.md)。

## 行为准则

参与本项目即表示你同意遵守 [行为准则](CODE_OF_CONDUCT.md)。

## 许可证

本项目以 [Apache-2.0](LICENSE) 开源；第三方组件声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。