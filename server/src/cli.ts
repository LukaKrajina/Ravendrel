#!/usr/bin/env node
import * as fs from 'fs';
import * as net from 'net';
import * as http from 'http';
import * as path from 'path';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { SemanticAnalyzer } from './semantic';
import { IRGenerator } from './ir';
import { RavendrelApiRouter } from './apiRouter';
import { VCGenerator } from './vcgen';
import { Cmd, PROTOCOL_VERSION, DAEMON_PORT, encodeFrame, decodeFrames } from './protocol';
import { packMMI, collectModuleInfo, readMMIExports } from './mmi';
import { CompileCache } from './cache';
import { buildMir } from './mir';
import { serializeMir } from './mir-serialize';
import { migrate, detectLang, SourceLang } from './migrate';
import { spawnSync } from 'child_process';

const DAEMON_TIMEOUT_MS = 30000; // daemon 响应超时(收到数据即重置)

// ─── 模型容器转换（rvdl export / rvdl import）────────────────────────────
// 连接 daemon 发 CMD_EXPORT_MODEL / CMD_IMPORT_MODEL，打印应答帧后退出。
// 与 run/compile 共用同一套帧协议与超时纪律；应答含 `RESPONSE: ERROR` 时
// 以非零码退出（脚本可据此判断成败，不必解析文本）。
function daemonModelConvert(verb: string, args: string): void {
    const client = net.createConnection({ host: '127.0.0.1', port: DAEMON_PORT }, () => {
        client.write(encodeFrame(Cmd.HELLO, PROTOCOL_VERSION));
        const cmd = verb.startsWith('IMPORT') ? Cmd.IMPORT_MODEL : Cmd.EXPORT_MODEL;
        client.write(encodeFrame(cmd, `${verb} ${args}`.trim()));
        client.write(encodeFrame(Cmd.EXIT));
    });

    let timeout: NodeJS.Timeout | null = null;
    const resetTimeout = () => {
        if (timeout) clearTimeout(timeout);
        timeout = setTimeout(() => {
            console.error(`[Ravendrel CLI] Daemon response timeout. Is 'runtime --daemon' responsive?`);
            client.destroy();
            process.exit(1);
        }, DAEMON_TIMEOUT_MS);
    };
    resetTimeout();

    let hadError = false;
    let recvBuffer: Buffer = Buffer.alloc(0);
    client.on('data', (data: Buffer) => {
        resetTimeout();
        recvBuffer = Buffer.concat([recvBuffer, data]);
        const { frames, rest } = decodeFrames(recvBuffer);
        recvBuffer = rest;
        for (const f of frames) {
            process.stdout.write(f);
            if (f.includes('RESPONSE: ERROR')) hadError = true;
        }
    });

    client.on('end', () => {
        if (timeout) clearTimeout(timeout);
        process.exit(hadError ? 1 : 0);
    });

    client.on('error', () => {
        if (timeout) clearTimeout(timeout);
        console.error(`[Ravendrel CLI] Failed to connect to Ravendrel Daemon on port ${DAEMON_PORT}. Is 'runtime --daemon' running?`);
        process.exit(1);
    });
}

// 定位 scripts/build-apk.sh：优先 RVDL_ROOT 环境变量，其次从当前工作目录向上查找
// （源码树内运行），最后从 cli.js 位置向上查找。
function locateBuildApkScript(): string {
    if (process.env.RVDL_ROOT) {
        const p = path.join(process.env.RVDL_ROOT, 'scripts', 'build-apk.sh');
        if (fs.existsSync(p)) return p;
    }
    // 从 cwd 向上（用户在源码树内执行 `rvdl build apk`）
    let dir = process.cwd();
    for (let i = 0; i < 6; i++) {
        const p = path.join(dir, 'scripts', 'build-apk.sh');
        if (fs.existsSync(p)) return p;
        dir = path.dirname(dir);
    }
    // 从 cli.js 位置向上
    dir = __dirname;
    for (let i = 0; i < 6; i++) {
        const p = path.join(dir, 'scripts', 'build-apk.sh');
        if (fs.existsSync(p)) return p;
        dir = path.dirname(dir);
    }
    return '';
}

function expandIncludes(src: string, baseDir: string, stack: Set<string>): string {
    const out: string[] = [];
    for (const line of src.split('\n')) {
        const m = line.match(/^\s*include\s+"([^"]+)"\s*;\s*$/);
        if (!m) { out.push(line); continue; }
        const incPath = path.resolve(baseDir, m[1]);
        if (stack.has(incPath)) throw new Error(`[Ravendrel CLI] Circular include: ${incPath}`);
        const incSrc = fs.readFileSync(incPath, 'utf-8');
        const next = new Set(stack); next.add(incPath);
        out.push(`// ===== include: ${m[1]} =====`);
        out.push(expandIncludes(incSrc, path.dirname(incPath), next));
        out.push(`// ===== end include: ${m[1]} =====`);
    }
    return out.join('\n');
}

function main() {
    const args = process.argv.slice(2);
    if (args.length === 0) {
        console.error("Usage:\n  rvdl run <script.rvdl>\n  rvdl verify <script.rvdl>\n  rvdl compile <arch> <mode> <script.rvdl>");
        console.error("  rvdl verify <script.rvdl>                  Statically verify contracts (requires/ensures/invariant)");
        console.error("  rvdl verify <script.rvdl> --smt [out.smt2] Export SMT-LIB for external solvers (Z3/cvc5)");
        console.error("  rvdl compile <arch> <mode> <script.rvdl> [-o <out>]   Compile to native binary (arch: x32|x64|arm64|android, mode: -e|-m; output defaults to <script dir>/<name>)");
        console.error("  rvdl serve <model.rvdlm> [--port <port>]   Launch an HTTP/WebSocket server for model inference");
        console.error("  rvdl ir <script.rvdl>                      Emit LLVM IR to stdout (no daemon connection)");
        console.error("  rvdl migrate <file> --from <lang> [-o o] Migrate existing QPL source to .rvdl (openqasm2/3, qsharp, quil, silq)");
        console.error("  rvdl export <safetensors|gguf> <in.rvdlm> <out> [tensor] [f32|f64|i32|i64|u8]");
        console.error("                                               Export .rvdlm weights to an ecosystem container (element type must be given)");
        console.error("  rvdl import <safetensors|gguf> <in> <out.rvdlm> [tensor]");
        console.error("                                               Import an ecosystem container into .rvdlm (element type is taken from the container)");
        console.error("  rvdl build apk <script.rvdl>               Build Android APK from .rvdl (AOT + Gradle)");
        process.exit(1);
    }

    const command = args[0];
    let filePath = "";
    let arch = "";
    let mode = "";
    let outputName = "";
    let smtMode = false;
    let smtOutput = "";
    let mmiOutput = "";
    let nativeLibs: string[] = [];
    let useMir = false; // --mir：走 MIR 下沉路径（COMPILE_MIR），默认字符串 IR
    
    if (command === 'run') {
        if (args.length < 2) {
            console.error("Usage: rvdl run <script.rvdl> [--native <lib> ...]");
            process.exit(1);
        }
        filePath = path.resolve(args[1]);
        // 解析 --native <path>：加载原生动态库
        const nIdx = args.indexOf('--native');
        if (nIdx !== -1) {
            for (let i = nIdx + 1; i < args.length; i++) {
                if (args[i].startsWith('-')) break;
                nativeLibs.push(path.resolve(args[i]));
            }
        }
        // --mir：走 MIR 下沉路径（COMPILE_MIR），默认字符串 IR
        if (args.includes('--mir')) useMir = true;
    } else if (command === 'ir') {
        if (args.length < 2) {
            console.error("Usage: rvdl ir <script.rvdl>");
            process.exit(1);
        }
        filePath = path.resolve(args[1]);
    } else if (command === 'mmi') {
        if (args.length < 2) {
            console.error("Usage: rvdl mmi <script.rvdl> [-o <out.rvdli>]");
            process.exit(1);
        }
        filePath = path.resolve(args[1]);
        const oIdx = args.indexOf('-o');
        if (oIdx !== -1 && args[oIdx + 1]) {
            mmiOutput = path.resolve(args[oIdx + 1]);
        } else {
            mmiOutput = path.join(path.dirname(filePath), path.parse(filePath).name + '.rvdli');
        }
    } else if (command === 'compile') {
        if (args.length < 4) {
            console.error("Usage: rvdl compile <x32|x64|arm64|android> <-e|-m> <script.rvdl> [-o <output>]");
            process.exit(1);
        }
        arch = args[1];
        mode = args[2];
        filePath = path.resolve(args[3]);
        
        if (!['x32', 'x64', 'arm64', 'android', 'arm64-android'].includes(arch)) {
            console.error(`[Ravendrel CLI] Error: Unsupported architecture '${arch}'. Use x32, x64, arm64, or android.`);
            process.exit(1);
        }

        if (!['-e', '-m'].includes(mode)) {
            console.error(`[Ravendrel CLI] Error: Unsupported mode '${mode}'. Use -e (executable) or -m (hybrid library).`);
            process.exit(1);
        }

        // 输出路径：默认写到源文件同目录（绝对路径——daemon 的 CWD 不可依赖）；
        // -o <path> 可指定输出路径基名（目标后缀由 arch 决定，如 android → .so）。
        const outIdx = args.indexOf('-o');
        if (outIdx !== -1 && args[outIdx + 1]) {
            outputName = path.resolve(args[outIdx + 1]);
        } else {
            outputName = path.join(path.dirname(filePath), path.parse(filePath).name);
        }
    } else if (command === 'verify') {
        if (args.length < 2) {
            console.error("Usage: rvdl verify <script.rvdl> [--smt [output.smt2]]");
            process.exit(1);
        }
        filePath = path.resolve(args[1]);
        const smtIdx = args.indexOf('--smt');
        if (smtIdx !== -1) {
            smtMode = true;
            const next = args[smtIdx + 1];
            if (next && !next.startsWith('--')) {
                smtOutput = path.resolve(next);
            }
        }
    } else if(command === 'serve'){
        if (args.length < 2) {
            console.error("Usage: rvdl serve <model.rvdlm> [--port <port>] [--module <vision.rvdli>] [--host <ip>] [--api-key <key>] [--rate-limit <n/min>] [--audit-log <file>]");
            process.exit(1);
        }
        
        const modelPath = path.resolve(args[1]);
        if (!fs.existsSync(modelPath)) {
            console.error(`[Ravendrel CLI] Error: Model file not found -> ${modelPath}`);
            process.exit(1);
        }

        // 可选多模态模块（vision 版 .rvdli，如 jarvis_vision.rvdli）：图像请求
        // 到达时 serve 经 BIND_MMI 把它绑进 daemon 主 JIT，生成脚本以字面量参数
        // 调 jarvis_forward_mm_i32。缺省时图像请求返回 400 与可读错误。
        let modulePath = '';
        const moduleIdx = args.indexOf('--module');
        if (moduleIdx !== -1 && args[moduleIdx + 1]) {
            modulePath = path.resolve(args[moduleIdx + 1]);
            if (!fs.existsSync(modulePath)) {
                console.error(`[Ravendrel CLI] Error: Vision module not found -> ${modulePath}`);
                process.exit(1);
            }
            console.log(`[Ravendrel Serve] Vision module '${path.basename(modulePath)}' (multimodal inference enabled)`);
        }

        // 端口优先级：--port 参数 > RVDL_SERVE_PORT 环境变量 > 默认 9080。
        // 曾经只有 --port 一个入口 —— 容器化/脚本化部署（无法改命令行）时
        // 只能改默认值，而那会影响所有用户。
        let port = 9080;
        if (process.env.RVDL_SERVE_PORT) {
            const envPort = parseInt(process.env.RVDL_SERVE_PORT, 10);
            if (!Number.isNaN(envPort) && envPort > 0 && envPort < 65536) port = envPort;
        }
        const portIdx = args.indexOf('--port');
        if (portIdx !== -1 && args[portIdx + 1]) {
            port = parseInt(args[portIdx + 1], 10);
        }

        // ── 安全选项（部署到公网时必须设置 api-key；默认只监听本机）────────
        // 优先级：命令行参数 > 环境变量 > 默认值。
        let host = process.env.RVDL_SERVE_HOST || '127.0.0.1';
        const hostIdx = args.indexOf('--host');
        if (hostIdx !== -1 && args[hostIdx + 1]) host = args[hostIdx + 1];

        let apiKey = process.env.RVDL_SERVE_KEY || '';
        const keyIdx = args.indexOf('--api-key');
        if (keyIdx !== -1 && args[keyIdx + 1]) apiKey = args[keyIdx + 1];

        let rateLimitPerMin = process.env.RVDL_SERVE_RATE
            ? parseInt(process.env.RVDL_SERVE_RATE, 10)
            : 60;
        const rateIdx = args.indexOf('--rate-limit');
        if (rateIdx !== -1 && args[rateIdx + 1]) rateLimitPerMin = parseInt(args[rateIdx + 1], 10);
        if (Number.isNaN(rateLimitPerMin)) rateLimitPerMin = 60;

        let auditLogPath = process.env.RVDL_SERVE_AUDIT || '';
        const auditIdx = args.indexOf('--audit-log');
        if (auditIdx !== -1 && args[auditIdx + 1]) auditLogPath = path.resolve(args[auditIdx + 1]);

        console.log(`[Ravendrel Serve] Pre-loading model '${path.basename(modelPath)}'...`);

        const router = new RavendrelApiRouter(modelPath, modulePath, { apiKey, rateLimitPerMin, auditLogPath });
        const server = http.createServer((req, res) => router.handleRequest(req, res)); //

        server.listen(port, host, () => {
            console.log(`[Ravendrel Serve] Inference server listening at http://${host}:${port}`);
            console.log(`[Ravendrel Serve] Security: auth=${apiKey ? 'on (Bearer / x-api-key)' : 'off (local only)'}, rateLimit=${rateLimitPerMin > 0 ? rateLimitPerMin + '/min/IP' : 'off'}${auditLogPath ? ', audit=' + auditLogPath : ''}`);
            console.log(`[Ravendrel Serve] API Router Endpoints active:`);
            console.log(`               - GET  http://${host}:${port}/v1/models`);
            console.log(`               - POST http://${host}:${port}/v1/chat/completions`);
            console.log(`               - POST http://${host}:${port}/v1/embeddings`);
        });
        
        return;
    } else if (command === 'migrate') {
        // rvdl migrate <file> [--from <openqasm2|openqasm3|qsharp|quil|silq>] [-o <out.rvdl>]
        // 把现有量子编程语言源码迁移为 .rvdl 源（source-to-source 转译）。
        if (args.length < 2) {
            console.error("Usage: rvdl migrate <file> [--from <openqasm2|openqasm3|qsharp|quil|silq>] [-o <out.rvdl>]");
            process.exit(1);
        }
        const srcPath = path.resolve(args[1]);
        if (!fs.existsSync(srcPath)) {
            console.error(`[Ravendrel CLI] Error: File not found -> ${srcPath}`);
            process.exit(1);
        }
        const src = fs.readFileSync(srcPath, 'utf-8');

        let lang: SourceLang | null = null;
        const fromIdx = args.indexOf('--from');
        if (fromIdx !== -1 && args[fromIdx + 1]) {
            lang = args[fromIdx + 1] as SourceLang;
        } else {
            lang = detectLang(src);
            if (!lang) {
                console.error("[Ravendrel CLI] Error: Cannot detect source language. Use --from <openqasm2|openqasm3|qsharp|quil|silq>.");
                process.exit(1);
            }
        }

        const outPath = (() => {
            const oIdx = args.indexOf('-o');
            if (oIdx !== -1 && args[oIdx + 1]) return path.resolve(args[oIdx + 1]);
            return path.join(path.dirname(srcPath), path.parse(srcPath).name + '.rvdl');
        })();

        const qkSrc = migrate(src, lang);
        fs.writeFileSync(outPath, qkSrc);
        console.log(`[Ravendrel Migrate] ${path.basename(srcPath)} (${lang}) -> ${outPath}`);
        return;
    } else if (command === 'export' || command === 'import') {
        // rvdl export <safetensors|gguf> <in.rvdlm> <out> [tensor] [f32|f64|i32|i64|u8]
        // rvdl import <safetensors|gguf> <in> <out.rvdlm> [tensor]
        //
        // 导出的类型参数是**必须显式给或默认 f32** 的：`.rvdlm` 头部不记录
        // 元素类型，按错的类型重组不会报错，只会得到垃圾（应答会回显实际类型）。
        // 导入不需要：容器自带 dtype，daemon 按容器 dtype 推导。
        const typeHint = command === 'export' ? ' [f32|f64|i32|i64|u8]' : '';
        if (args.length < 4) {
            console.error(`Usage: rvdl ${command} <safetensors|gguf> <in> <out> [tensor]${typeHint}`);
            process.exit(1);
        }
        const fmt = args[1];
        if (fmt !== 'safetensors' && fmt !== 'gguf') {
            console.error(`[Ravendrel CLI] Error: Unsupported container '${fmt}'. Use safetensors or gguf.`);
            process.exit(1);
        }
        const inPath = path.resolve(args[2]);
        const outPath = path.resolve(args[3]);
        if (!fs.existsSync(inPath)) {
            console.error(`[Ravendrel CLI] Error: File not found -> ${inPath}`);
            process.exit(1);
        }
        let tensor = '';
        let type = '';
        if (command === 'export') {
            tensor = args[4] ?? '';
            type = args[5] ?? '';
            if (type && !['f32', 'f64', 'i32', 'i64', 'u8'].includes(type)) {
                console.error(`[Ravendrel CLI] Error: Unsupported element type '${type}'. Use f32|f64|i32|i64|u8.`);
                process.exit(1);
            }
        } else {
            tensor = args[4] ?? '';
        }
        const verb = `${command === 'export' ? 'EXPORT' : 'IMPORT'}_${fmt === 'gguf' ? 'GGUF' : 'SAFETENSORS'}`;
        const rest = [inPath, outPath, tensor, type].filter(s => s.length > 0).join(' ');
        console.log(`[Ravendrel ${command === 'export' ? 'Export' : 'Import'}] ${verb} ${rest}`);
        daemonModelConvert(verb, rest);
        return;
    } else if (command === 'build') {
        // rvdl build apk <file.rvdl>：把 .rvdl 项目构建成 Android APK（AOT 编译 + Gradle 打包）。
        if (args[1] !== 'apk') {
            console.error("[Ravendrel CLI] Error: Unknown build target. Use 'rvdl build apk <file.rvdl>'.");
            process.exit(1);
        }
        if (args.length < 3) {
            console.error("Usage: rvdl build apk <file.rvdl>");
            process.exit(1);
        }
        const qkFile = path.resolve(args[2]);
        if (!fs.existsSync(qkFile)) {
            console.error(`[Ravendrel CLI] Error: File not found -> ${qkFile}`);
            process.exit(1);
        }
        const script = locateBuildApkScript();
        if (!script) {
            console.error("[Ravendrel CLI] Error: 未找到 scripts/build-apk.sh。请设置 RVDL_ROOT 指向 Ravendrel 源码根目录，或在源码树内运行。");
            process.exit(1);
        }
        console.log(`[Ravendrel Build] 构建 Android APK: ${qkFile}`);
        // 透传额外参数（--release / --debug 等）给 build-apk.sh
        const extraArgs = args.slice(3).filter(a => a.startsWith('--'));
        const r = spawnSync('bash', [script, qkFile, ...extraArgs], { stdio: 'inherit' });
        process.exit(r.status ?? 1);
    } else {
        console.error(`[Ravendrel CLI] Error: Unknown command '${command}'`);
        console.error("Usage:\n  rvdl run <script.rvdl>\n  rvdl compile <arch> <mode> <script.rvdl>");
        process.exit(1);
    }

    if (!fs.existsSync(filePath)) {
        console.error(`[Ravendrel CLI] Error: File not found -> ${filePath}`);
        process.exit(1);
    }

    let sourceCode = fs.readFileSync(filePath, 'utf-8');

    try {
        sourceCode = expandIncludes(sourceCode, path.dirname(filePath), new Set([filePath]));
        const lexer = new Lexer(sourceCode);
        const parser = new Parser(lexer);
        const ast = parser.parse();

        const analyzer = new SemanticAnalyzer();
        analyzer.analyze(ast);

        if (analyzer.errors.length > 0) {
            analyzer.errors.forEach(err => console.error(`[Ravendrel Semantic Error] Line ${err.line}: ${err.message}`));
            process.exit(1);
        }

        // 警告不退出（W-SHU001 等降级告警照常执行），但必须让用户看见
        analyzer.warnings.forEach(w => console.error(`[Ravendrel Warning] Line ${w.line}: ${w.message}`));

        // 解析 import：读取 .rvdli 导出签名，生成 importSignatures + importList
        const importSignatures = new Map<string, Map<string, { params: string[]; ret: string }>>();
        const importList: { alias: string; path: string }[] = [];
        for (const node of ast.body) {
            if ((node as any).type === 'ImportDecl') {
                const imp = node as any;
                const sigs = readMMIExports(imp.path, path.dirname(filePath));
                const funcs = new Map<string, { params: string[]; ret: string }>();
                for (const e of sigs) funcs.set(e.name, { params: e.params, ret: e.ret });
                importSignatures.set(imp.alias, funcs);
                importList.push({ alias: imp.alias, path: path.resolve(path.dirname(filePath), imp.path) });
            }
        }

        // 增量编译缓存：以「源内容 + import 依赖 mtime」为 key，命中则复用 IR，
        // 跳过 IRGenerator 重生成。仅在缓存未命中时全量生成并回写。
        const cache = new CompileCache(path.join(path.dirname(filePath), '.rvdl-cache'));
        const deps: { path: string; mtimeMs: number }[] = [];
        for (const imp of importList) {
            try {
                deps.push({ path: imp.path, mtimeMs: fs.statSync(imp.path).mtimeMs });
            } catch {
                deps.push({ path: imp.path, mtimeMs: 0 });
            }
        }
        let llvmIR = cache.get(filePath, sourceCode, deps);
        if (!llvmIR) {
            const irGen = new IRGenerator();
            llvmIR = irGen.generate(ast, importSignatures.size > 0 ? importSignatures : undefined);
            cache.set(filePath, sourceCode, deps, llvmIR);
        }

        // IR 导出模式：只输出 LLVM IR 到 stdout，不连接 daemon（供 C++ embedded JIT 消费）
        if (command === 'ir') {
            process.stdout.write(llvmIR);
            process.exit(0);
        }

        // MMI 打包模式：rvdl mmi <script.rvdl> [-o <out.rvdli>]
        if (command === 'mmi') {
            const { exports, permissions, imports } = collectModuleInfo(ast);
            // 模块版本随工具链版本（package.json 单一来源）。曾经这里写死
            // '1.0.0'，与 package.json / 发布脚本三处各自漂移 —— 模块头里的
            // 版本对不上打包它的工具，且没有任何一步会报错。
            const toolVersion: string = require('../../package.json').version;
            const header = { name: path.parse(filePath).name, version: toolVersion, exports, permissions, imports };
            const buffer = packMMI(header, llvmIR);
            fs.writeFileSync(mmiOutput, buffer);
            console.log(`[Ravendrel MMI] Packed ${exports.length} export(s) (v${toolVersion}) -> ${mmiOutput}`);
            process.exit(0);
        }

        const vcGen = new VCGenerator();
        const obligations = vcGen.generate(ast);
        const vcProtocol = obligations.length > 0 ? vcGen.toProtocol(obligations) : null;

        // SMT-LIB 导出模式：不连接 daemon，直接导出验证条件供外部求解器判定
        if (command === 'verify' && smtMode) {
            if (obligations.length === 0) {
                console.log("[Ravendrel Verify] No contracts (requires/ensures/invariant) found. Nothing to export.");
                process.exit(0);
            }
            const smtLib = vcGen.toSmtLib(obligations);
            if (smtOutput) {
                fs.writeFileSync(smtOutput, smtLib);
                console.log(`[Ravendrel Verify] SMT-LIB exported to ${smtOutput}`);
            } else {
                process.stdout.write(smtLib + "\n");
            }
            process.exit(0);
        }

        const client = net.createConnection({ host: '127.0.0.1', port: DAEMON_PORT }, () => {
            client.write(encodeFrame(Cmd.HELLO, PROTOCOL_VERSION));
            if (command === 'run') {
                for (const lib of nativeLibs) {
                    client.write(encodeFrame(Cmd.LOAD_NATIVE, lib));
                }
                for (const imp of importList) {
                    client.write(encodeFrame(Cmd.BIND_MMI, `${imp.alias} ${imp.path}`));
                }
                // --mir：走 MIR 下沉路径（daemon 端 MirModuleBuilder 构建 Module）；默认字符串 IR
                if (useMir) {
                    const mirJson = serializeMir(buildMir(ast));
                    client.write(encodeFrame(Cmd.COMPILE_MIR, mirJson));
                } else {
                    client.write(encodeFrame(Cmd.COMPILE, llvmIR));
                }
                if (vcProtocol) {
                    client.write(encodeFrame(Cmd.VERIFY, vcProtocol));
                }
                // 多维标签函数：带 @layer 块走拓扑调度入口；否则走脚本模式 rvdl_main。
                const hasLayerFns = ast.body.some(n => (n as any).type === 'FunctionDeclaration' && (n as any).layer);
                if (hasLayerFns) {
                    client.write(encodeFrame(Cmd.EXECUTE_TOPOLOGY, 'int32 rvdl_topology_entry'));
                } else {
                    client.write(encodeFrame(Cmd.EXECUTE, 'int32 rvdl_main'));
                }
            } else if (command === 'compile') {
                // Android 交叉编译：把 CLI 进程的工具链环境变量随请求转发给 daemon
                // （daemon 可能在不同环境中启动，不能依赖其继承 RVDL_ANDROID_*）。
                const toolchainTokens: string[] = [];
                if (arch === 'android' || arch === 'arm64-android') {
                    const envMap: Array<[string, string | undefined]> = [
                        ['clangxx', process.env.RVDL_ANDROID_CLANGXX],
                        ['sysroot', process.env.RVDL_ANDROID_SYSROOT],
                        ['rtdir', process.env.RVDL_ANDROID_RT_DIR],
                        ['extra_libs', process.env.RVDL_AOT_EXTRA_LIBS],
                    ];
                    for (const [k, v] of envMap) {
                        if (!v) continue;
                        // 头行是空白分隔的 token 列表：extra_libs 的空格用 '|' 编码。
                        toolchainTokens.push(`${k}=${k === 'extra_libs' ? v.trim().replace(/ +/g, '|') : v}`);
                    }
                }
                const aotHead = [arch, mode, outputName, ...toolchainTokens].join(' ');
                client.write(encodeFrame(Cmd.AOT_COMPILE, `${aotHead}\n${llvmIR}`));
            } else if (command === 'verify') {
                if (vcProtocol) {
                    client.write(encodeFrame(Cmd.VERIFY, vcProtocol));
                } else {
                    console.log("[Ravendrel Verify] No contracts (requires/ensures/invariant) found. Nothing to verify.");
                }
            }
            client.write(encodeFrame(Cmd.EXIT));
        });

        // daemon 响应超时:收到数据即重置从而避免挂起
        let timeout: NodeJS.Timeout | null = null;
        const resetTimeout = () => {
            if (timeout) clearTimeout(timeout);
            timeout = setTimeout(() => {
                console.error(`[Ravendrel CLI] Daemon response timeout. Is 'runtime --daemon' responsive?`);
                client.destroy();
                process.exit(1);
            }, DAEMON_TIMEOUT_MS);
        };
        resetTimeout();

        let recvBuffer: Buffer = Buffer.alloc(0);
        client.on('data', (data: Buffer) => {
            resetTimeout();
            recvBuffer = Buffer.concat([recvBuffer, data]);
            const { frames, rest } = decodeFrames(recvBuffer);
            recvBuffer = rest;
            for (const f of frames) process.stdout.write(f);
        });

        client.on('end', () => {
            if (timeout) clearTimeout(timeout);
            process.exit(0);
        });

        client.on('error', (err) => {
            if (timeout) clearTimeout(timeout);
            console.error(`[Ravendrel CLI] Failed to connect to Ravendrel Daemon on port ${DAEMON_PORT}. Is 'runtime --daemon' running?`);
            process.exit(1);
        });

    } catch (error: any) {
        console.error(`[Ravendrel System Error] ${error.message}`);
        process.exit(1);
    }
}

main();