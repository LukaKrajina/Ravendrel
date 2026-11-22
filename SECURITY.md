# 安全策略 · Security Policy

OpenMasons 重视 Ravendrel 及其工具链的安全性。

## 支持范围

| 版本 | 是否接受安全报告 |
| --- | --- |
| 最新发布版本（`package.json` 中的 `version`） | ✅ |
| 更早版本 | ❌ 请先升级到最新版再评估 |

## 报告漏洞

请使用 GitHub 仓库的 **Security → Report a vulnerability**（私有 Security Advisory）提交报告，不要新建公开 Issue。

为提高处理效率，请包含：

- 受影响组件（`server/` 语言服务器、`runtime/` 运行时、`chatWebUI/`、`installer/` 等）与版本
- 复现步骤、最小测试用例、影响范围
- 可能的修复建议（可选）

我们会在 **7 天内**确认收到，并在验证后协调修复与披露时间。请在我们发布修复前不要公开细节。

## 敏感文件处理

- **切勿提交**代码签名私钥（`*.pfx`、`*.key`）、Android `release.keystore`、`keystore.properties` 或含凭据的 `local.properties`。这些路径已在 [.gitignore](.gitignore) 中排除。
- 自签名证书与 WDAC 策略仅用于本机开发环境；若怀疑私钥泄露，请立即在 GitHub Security Advisory 中报告，我们会协助轮换。
- 公钥证书（`.cer`）可以随仓库分发，用于校验构建来源。

## 依赖安全

第三方组件及其许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。若发现上游依赖漏洞，请同时说明受影响的版本范围与本项目的使用位置。