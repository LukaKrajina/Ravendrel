# 脚本目录 · Scripts

本目录集中管理 Ravendrel 的构建、安装、打包、部署与代码生成脚本。所有脚本均以自身位置推导仓库根目录，可从任意工作目录调用。

## 目录结构

```
scripts/
├── build/
│   ├── windows/       Windows 原生构建：build.bat、build-inc.cmd、build-runtime-windows.ps1、
│   │                  build-kokkos-windows.ps1、build-qt-md-mt.ps1、prepare-vsix-runtime.ps1
│   ├── linux/         Linux 构建与交叉编译：build-linux.sh、build-linux-wsl.sh、
│   │                  build-windows-cross.sh、prepare-vsix-runtime.sh、mingw-w64-toolchain.cmake
│   ├── android/       build-apk.sh（AOT 交叉编译 + Gradle 打包）
│   ├── qcos/          可引导内核：build-qcos-kernel.ps1、build-qcos-iso.ps1、build-grub-img.ps1
│   ├── check/         check-rt-symbols.sh（ABI 符号自检，CI 使用）
│   ├── wdac/          WDAC 策略部署与刷新（含 backup/ 历史策略）
│   └── esbuild.mjs    TypeScript bundle 入口（由 npm run compile 调用）
├── package/
│   ├── vsix/          package-vsix.sh、package-vsix-universal.sh（VSCode 扩展打包）
│   └── release/       package-release-linux.sh、package-release.ps1（发布归档）
├── install/           install-linux.sh、install-system.sh（含 PATH 注册与卸载）
├── generate/          gen_mldsa_kat.py、gen_mlkem_kat.py、gen_unihan_pinyin.py、translate_i18n.mjs
├── sign/              sign.ps1（OpenMasons 自签名证书）
└── certs/             签名证书（*.pfx 私钥严禁提交，已在 .gitignore 排除）
```

## 常用入口

| 目的 | 命令（仓库根目录执行） |
| --- | --- |
| 编译 TypeScript 扩展 | `npm run compile` |
| 构建 Linux 运行时 | `bash scripts/build/linux/build-linux.sh` |
| 构建 Windows 运行时 | `scripts\build\windows\build.bat` |
| 增量构建 Windows | `scripts\build\windows\build-inc.cmd` |
| 构建 Android APK | `bash scripts/build/android/build-apk.sh <file.rvdl>` |
| 打 VSIX（Linux） | `bash scripts/package/vsix/package-vsix.sh --linux` |
| 打通用 VSIX | `bash scripts/package/vsix/package-vsix-universal.sh` |
| 安装到用户目录 | `bash scripts/install/install-system.sh` |
| 校验 ABI 符号 | `bash scripts/build/check/check-rt-symbols.sh` |
| 部署 WDAC 策略 | `.\scripts\build\wdac\deploy-wdac.ps1`（管理员） |

完整构建说明见 [README.md](../README.md#-依赖与构建)。