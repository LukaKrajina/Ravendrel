# Third-Party Notices / 第三方组件协议

本项目使用了以下第三方开源组件。这些组件归其各自作者所有，并遵循各自的许可证。
本项目（Ravendrel）本身以 MIT 许可证发布，详见根目录 [LICENSE](./LICENSE)。

The Ravendrel project is distributed under the MIT License (see [LICENSE](./LICENSE)).
The following third-party components are distributed under their own licenses.

## Node.js / VS Code 扩展

| 包 Package | 许可证 License |
| --- | --- |
| vscode-languageclient | MIT |
| vscode-languageserver | MIT |
| vscode-languageserver-textdocument | MIT |
| @types/node | MIT |
| @types/vscode | MIT |
| @types/mocha | MIT |
| @vscode/test-electron | MIT |
| mocha | MIT |
| typescript | Apache-2.0 |

## Web UI（chatWebUI）

| 包 Package | 许可证 License |
| --- | --- |
| vite | MIT |
| vite-plugin-pwa | MIT |
| tailwindcss | MIT |
| @tailwindcss/vite | MIT |
| autoprefixer | MIT |
| postcss | MIT |
| typescript | Apache-2.0 |
| dexie | Apache-2.0 |
| fake-indexeddb | Apache-2.0 |
| eslint | MIT |
| eslint-config-prettier | MIT |
| globals | MIT |
| prettier | MIT |
| typescript-eslint | MIT |
| vitest | MIT |

## 安装器 Installer（Go）

| 包 Package | 许可证 License |
| --- | --- |
| golang.org/x/sys | BSD-3-Clause |

## SE 编辑器 Editor（Electron）

| 包 Package | 许可证 License |
| --- | --- |
| electron | MIT |
| codemirror | MIT |
| @codemirror/lang-javascript | MIT |
| @codemirror/state | MIT |
| @codemirror/theme-one-dark | MIT |
| esbuild | MIT |
| eslint | MIT |
| eslint-config-prettier | MIT |
| globals | MIT |
| prettier | MIT |
| typescript-eslint | MIT |

## rvdlRSP 仿真平台 Simulation Platform（可选依赖 Optional）

| 组件 Component | 许可证 License |
| --- | --- |
| Qt 6 | LGPL-3.0（含 Qt-LGPL-exception）或商业许可（版权：The Qt Company，仅在构建 `rvdlRSP_gui` 时动态链接） |
| OpenCV | Apache-2.0（仅在启用 `RVDLRSP_USE_OPENCV` 时链接） |
| stb_image | MIT 或 Unlicense（公共领域）双许可（版权：Sean Barrett，仅在 `vendor/stb/` 放置时使用） |

## C++ 运行时 Runtime（C++20）

| 组件 Component | 许可证 License |
| --- | --- |
| LLVM | Apache-2.0 WITH LLVM-exception |
| Kokkos | BSD-3-Clause（部分捆绑组件遵循各自宽松许可证） |
| Vulkan SDK / Vulkan-Headers | Apache-2.0（The Khronos Group Inc.） |
| pq-crystals Kyber（ML-KEM-768） | Public Domain / CC0（`runtime/include/qchain/primitives/ml_kem.hpp` 移植自 pq-crystals/kyber 参考实现） |
| pq-crystals Dilithium（ML-DSA-65） | Public Domain / CC0（`runtime/include/qchain/primitives/ml_dsa.hpp` 移植自 pq-crystals/dilithium 参考实现） |
| tiny_sha3（Keccak-f[1600]） | Public Domain（`runtime/include/qchain/primitives/sha3.hpp` 参考公有领域 tiny_sha3 实现） |

## vendored 库 Vendored libraries

| 组件 Component | 位置 Location | 许可证 License |
| --- | --- | --- |
| GLFW | `vendor/GLFW/` | zlib/libpng License（版权：Marcus Geelnard、Camilla Löwy，详见 `vendor/GLFW/LICENSE.md`） |
| Nuklear | `vendor/Nuklear/` | MIT 或 Unlicense（公共领域）双许可，任选其一（版权：Micha Mettke，详见 `vendor/Nuklear/LICENSE`） |
| Unicode Unihan 数据库（汉字拼音数据） | `vendor/pinyin/` | Unicode License v3（版权：Unicode, Inc.，详见 `vendor/pinyin/LICENSE`） |
| stb_image | `vendor/stb/` | MIT 或 Unlicense（公共领域）双许可（版权：Sean Barrett，图像加载：PNG/JPEG 解码） |

## 构建工具 Build tools（可选 Optional）

以下组件仅用于**本地构建**（生成可引导 QCOS 镜像、构建运行时），不随 Ravendrel 运行时或分发包一同分发：

| 组件 Component | 许可证 License |
| --- | --- |
| GRUB 2 | GPL-3.0（仅由 `build-qcos-iso.ps1` / `build-grub-img.ps1` 用于生成可引导 QCOS ISO / GRUB 镜像） |
| xorriso | GPL-3.0（仅由 `build-qcos-iso.ps1` 用于生成 ISO） |
| zlib | zlib License（LLVMSupport 的链接接口依赖） |
| zstd | BSD-3-Clause（LLVMSupport 的链接接口依赖） |
| Android SDK | Apache-2.0（版权：Google LLC，仅由 `scripts/build-apk.sh` 用于构建 Android APK，不随运行时分发） |
| Android NDK | Apache-2.0（版权：Google LLC；NDK 工具链内含 LLVM/Clang、binutils 等第三方组件，遵循各自许可证；仅用于 Android 交叉编译） |
| Gradle | Apache-2.0（版权：Gradle Inc.，仅由 `scripts/build-apk.sh` 用于打包 APK） |
| Android Gradle Plugin (AGP) | Apache-2.0（版权：Google LLC，仅用于 Gradle 打包 APK） |

## 说明 Notes

- 上述许可证文本以各组件随附的 `LICENSE` / `NOTICE` 文件为准；本文件仅作汇总说明。
- 使用、分发或再分发本项目时，请一并保留各第三方组件的版权与许可声明。