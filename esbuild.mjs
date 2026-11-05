// ============================================================================
// esbuild 打包：把 client / server 各自打成单文件 bundle，
// 消除 vsix 中的 node_modules 依赖，减小体积与加载时间。
//
//   client → client/out/extension.js（external: vscode，运行时模块由编辑器提供）
//   server → server/out/server.js（vscode-languageserver 等 npm 依赖全部内联）
// ============================================================================
import * as esbuild from 'esbuild';

const common = {
    bundle: true,
    format: 'cjs',
    platform: 'node',
    target: 'node18',
    sourcemap: false,
    minify: true,
    logLevel: 'info',
};

await Promise.all([
    esbuild.build({
        ...common,
        entryPoints: ['client/src/extension.ts'],
        outfile: 'client/out/extension.js',
        external: ['vscode'], // VSCode API 由编辑器运行时注入
    }),
    esbuild.build({
        ...common,
        entryPoints: ['server/src/server.ts'],
        outfile: 'server/out/server.js',
        // child_process / fs / path 是 Node 内建，esbuild 自动 external
    }),
    // rvdl 命令行（CLI）：打成单文件，供系统安装脚本部署到 PATH，
    // 不再依赖 server/out/ 下的其他 tsc 产物。
    // 注意：cli.ts 源码自带 `#!/usr/bin/env node`，esbuild 会原样保留，
    // 此处不要再加 banner，否则会出现两个 shebang 导致 node 解析失败。
    esbuild.build({
        ...common,
        entryPoints: ['server/src/cli.ts'],
        outfile: 'server/out/rvdl.js',
    }),
]);
