import {
    createConnection,
    TextDocuments,
    ProposedFeatures,
    InitializeParams,
    TextDocumentSyncKind,
    InitializeResult,
    CompletionItem,
    CompletionItemKind,
    Diagnostic,
    DiagnosticSeverity,
    SemanticTokensBuilder,
    Hover,
    MarkupKind,
    DocumentSymbol,
    SymbolKind,
    Location,
    Range,
    Position,
    SemanticTokensRequest,
    SemanticTokensParams
} from 'vscode-languageserver/node';

import { TextDocument } from 'vscode-languageserver-textdocument';
import { spawn } from 'child_process';
import * as fs from 'fs';
import * as path from 'path';
import * as net from 'net';
import { Lexer, TokenType } from './lexer';
import { Parser } from './parser';
import { SemanticAnalyzer } from './semantic';
import { IRGenerator } from './ir';
import { VCGenerator } from './vcgen';
import {
    KEYWORD_DOCS, TYPE_DOCS, FUNCTION_DOCS, ANNOTATION_DOCS,
    LAYER_KEY_DOCS, LAYER_TAG_KEY_NAMES, LAYER_TAG_INSERTS,
    layerKeyDoc, layerEnumValueDoc,
    renderHover, Lang,
} from './hover-docs';
import { insideLayerTagKeys, enclosingLayerTag, layerKeyBefore } from './layer-context';
import { migrate, detectLang } from './migrate';
import { Cmd, PROTOCOL_VERSION, DAEMON_PORT, encodeFrame, decodeFrames } from './protocol';

const connection = createConnection(ProposedFeatures.all);

const documents: TextDocuments<TextDocument> = new TextDocuments(TextDocument);

// 悬停提示语言：根据开发者系统语言（LSP Initialize 的 `locale`）自动选择中文或英文。
let hoverLang: Lang = 'zh';

// 悬停提示的分节 / 类别标签多语言文案
const HOVER_TEXT = {
    kind: {
        type: { zh: '类型', en: 'Type' },
        fn: { zh: '内置函数', en: 'Built-in function' },
        keyword: { zh: '关键字', en: 'Keyword' },
        annotation: { zh: '注解', en: 'Annotation' },
        layerKey: { zh: '@layer 键', en: '@layer key' },
        layerEnum: { zh: '枚举取值', en: 'Enum value' },
    },
    fallback: {
        keyword: { zh: '关键字', en: 'Keyword' },
        identifier: { zh: '标识符', en: 'Identifier' },
        number: { zh: '数字字面量', en: 'Number literal' },
        string: { zh: '字符串字面量', en: 'String literal' },
        annotation: { zh: '注解', en: 'Annotation' },
    },
} as const;

const t = (entry: { zh: string; en: string }): string => entry[hoverLang];

// 定位 runtime：优先扩展目录下的 bin/runtime（随 vsix 打包，开箱即用），
// 回退到 PATH 中的 ravendrel 命令（用户自行安装的 runtime）。
// 这样一箭双雕。
function resolveRuntime(): { path: string; env?: NodeJS.ProcessEnv } {
    const extRoot = path.resolve(__dirname, '..', '..');
    // 合并打包：bin/ 下按平台分子目录（linux-x64 / win32-x64 / darwin-x64），
    // 一个 vsix 同时携带多个平台的 runtime，安装后按 process.platform 选择。
    const platformDir = process.platform === 'win32' ? 'win32-x64'
        : process.platform === 'darwin' ? 'darwin-x64' : 'linux-x64';
    const binDir = path.join(extRoot, 'bin', platformDir);
    const binName = process.platform === 'win32' ? 'runtime.exe' : 'runtime';
    const bundled = path.join(binDir, binName);
    if (!fs.existsSync(bundled)) return { path: 'ravendrel' };

    // Linux / macOS：vsix（zip）不保留可执行位，安装后 runtime 往往是 0644，
    // 直接 spawn 会 EACCES；这里按需补上执行权限（失败不致命，回退到 PATH 里的 ravendrel）。
    if (process.platform !== 'win32') {
        try { fs.chmodSync(bundled, 0o755); } catch { /* 忽略：只读 FS 等情形 */ }
    }

    // Linux / macOS：让动态链接器在 bin/<platform>/ 里找到随 vsix 打包的 librvdl_rt.so，
    // 否则 spawn 时 `runtime` 会因缺依赖而启动失败。
    const env: NodeJS.ProcessEnv = { ...process.env };
    if (process.platform !== 'win32') {
        const key = process.platform === 'darwin' ? 'DYLD_LIBRARY_PATH' : 'LD_LIBRARY_PATH';
        const prev = process.env[key] || '';
        env[key] = prev ? `${binDir}${path.delimiter}${prev}` : binDir;
    }
    return { path: bundled, env };
}

// ─── daemon 执行（侧边栏与执行共享同一 QVM 快照）──────────────────────────

/** 尝试 ping daemon（连接成功即返回 true） */
function pingDaemon(timeoutMs = 500): Promise<boolean> {
    return new Promise(res => {
        const s = net.createConnection({ port: DAEMON_PORT, host: 'localhost' });
        const done = (ok: boolean) => { s.destroy(); res(ok); };
        s.on('connect', () => done(true));
        s.on('error', () => done(false));
        s.setTimeout(timeoutMs, () => done(false));
    });
}

/**
 * 确保 daemon 运行：先尝试连接，失败则 spawn `runtime --daemon` 并等待就绪。
 * 执行改走 daemon 后，用户代码的量子操作作用在 daemon 的共享 QVM 上，
 * 侧边栏通过 GET_SNAPSHOT 即可读到「刚执行代码」的量子对象 / 比特状态。
 */
async function ensureDaemon(): Promise<void> {
    if (await pingDaemon()) return;

    const rt = resolveRuntime();
    const daemon = spawn(rt.path, ['--daemon'], rt.env ? { env: rt.env } : {});
    daemon.stderr.on('data', d => connection.sendNotification('ravendrel/printConsole', `[Ravendrel Daemon] ${d.toString()}`));
    daemon.on('error', () => { /* 忽略，等待超时兜底 */ });

    for (let i = 0; i < 40; i++) {
        await new Promise(r => setTimeout(r, 250));
        if (await pingDaemon()) return;
    }
    throw new Error(`Failed to start Ravendrel daemon on port ${DAEMON_PORT}`);
}

/** 通过 daemon 发送一串命令帧，读取响应并逐帧回调。 */
function daemonRequest(frames: Buffer[], onOutput: (text: string) => void): Promise<void> {
    return new Promise((resolve, reject) => {
        const sock = net.createConnection({ port: DAEMON_PORT, host: 'localhost' });
        let recv: Buffer = Buffer.alloc(0);
        sock.on('connect', () => {
            sock.write(encodeFrame(Cmd.HELLO, PROTOCOL_VERSION));
            for (const f of frames) sock.write(f);
            sock.write(encodeFrame(Cmd.EXIT));
        });
        sock.on('data', (d: Buffer) => {
            recv = Buffer.concat([recv, d]);
            const { frames: parsed, rest } = decodeFrames(recv);
            recv = rest;
            for (const f of parsed) onOutput(f);
        });
        sock.on('close', () => resolve());
        sock.on('error', reject);
    });
}

// ─── RVDL 关键字 / 内置函数 / 类型表（用于补全、悬停、语义高亮）──────────
const CONTROL_KEYWORDS = [
    'if', 'else', 'while', 'for', 'return', 'break', 'continue',
    'spawn', 'entangle', 'spin', 'route', 'fuse', 'fallback',
    'new', 'auto', 'let', 'fn', 'result', 'unsafe'
];

const OTHER_KEYWORDS = [
    'mod', 'use', 'pub', 'form', 'impl', 'trait', 'template', 'rank', 'self',
    'lattice', 'export', 'import', 'extern', 'requires', 'ensures', 'invariant',
    'from', 'make', 'cap', 'native', 'fixed', 'flavor', 'morph', 'addr', 'basis_state', 'path'
];

const TYPE_KEYWORDS = new Set([
    'int', 'int8', 'int16', 'int32', 'int64',
    'uint8', 'uint16', 'uint32', 'uint64',
    'float', 'double', 'string', 'char', 'bool',
    'Qubit', 'QObject', 'QModel', 'QRegister', 'Result',
    'DiracState', 'BellState', 'QuantumRegister', 'QReservoir'
]);

const BUILTIN_FUNCTIONS = [
    'alloc', 'measure', 'measure_x', 'measure_y', 'encode_text', 'encode_image',
    'qlm_invoke', 'qlm_load', 'rvdl_encode_string', 'qlm_forward', 'rvdl_decode_string',
    'mind_read', 'mind_train', 'mind_feedback', 'veda_qlm_train',
    'h', 'x', 'rz', 'cnot', 'toffoli', 'swap', 'qft', 'iqft', 'braid',
    'cx', 'ch', 'crz', 'cswap', 'c_toffoli', 'cqft', 'cbraid',
    'surrogate', 'tanh_quantize', 'lif_step', 'mellowmax2', 'logsumexp2', 'boltzmann2',
    'tnorm_luk', 'tnorm_prod', 'tnorm_godel', 'polymer_weight', 'polymer_mix_bound',
    'tau_alpha', 'tau_generational', 'tau_fold_depth', 'tau_gear_ratio', 'tau_route_cost', 'tau_route_price',
    'qstate_purity', 'qstate_linear_entropy', 'qstate_renyi_entropy', 'math_dot', 'math_norm',
    'rvdl_sys_call', 'rvdl_sys_calld', 'rvdl_sys_log', 'rvdl_sys_logi', 'rvdl_sys_callp', 'rvdl_gc_free',
    'rvdl_qms_gap', 'rvdl_mix_bound', 'rvdl_qms_conc',
    'sync_load', 'sync_store', 'sync_add', 'sync_cas', 'sync_lock', 'sync_unlock',
    'outb', 'inb', 'outw', 'inw', 'outl', 'inl', 'rvdl_gc_alloc',
    'strlen', 'kglobals_addr',
    'qchain_wallet', 'qchain_mint', 'qchain_transfer', 'qchain_balance',
    'qchain_mine', 'qchain_height', 'qchain_verify', 'qchain_qkd', 'qchain_qdba',
    'qchain_coin_mint', 'qchain_coin_verify', 'qchain_sha3', 'qchain_hmac', 'qchain_hash_unicode',
    'qchain_sign', 'qchain_sign_verify', 'qchain_sign_pubkey', 'qchain_mlkem_encaps',
    'qchain_mlkem_decaps', 'qchain_causal_verify', 'qchain_cipher_encrypt', 'qchain_cipher_decrypt'
];

// 注解标签：多维拓扑调度（@layer）+ 可逆编织门合成 + 量子物理特性 + 经典编译属性
const ANNOTATION_KEYWORDS = [
    '@layer', '@[gate]', '@[undo]', '@[steer]', '@[unitary]', '@[measure]',
    '@[coherence]', '@[noise]', '@[basis]', '@[decoherence_free]', '@[error_correction]',
    '@[inline]', '@[noinline]', '@[pure]', '@[readonly]', '@[cold]', '@[hot]',
    '@[noreturn]', '@[export]', '@[section]', '@[naked]'
];

// 量子门：在 lexer 中已不是关键字（语句/表达式位置按标识符处理），
// 补全与语义高亮共用此表。
const QUANTUM_GATE_NAMES = [
    'h', 'x', 'y', 'z', 's', 't', 'rz', 'rx', 'ry',
    'cnot', 'toffoli', 'swap', 'qft', 'iqft', 'braid',
    'measure_x', 'measure_y',
    'cx', 'ch', 'crz', 'cswap', 'c_toffoli', 'cqft', 'cbraid',
];
const QUANTUM_GATE_SET = new Set(QUANTUM_GATE_NAMES);

// ─── @layer(...) 的上下文定位：见 layer-context.ts（纯文本位置判定，单独成模块以便测试）───

connection.onInitialize((params: InitializeParams) => {
    // 根据开发者系统语言选择悬停提示语言：中文（zh*）→ 中文，其余（en* 等）→ 英文。
    hoverLang = ((params.locale ?? '').toLowerCase().startsWith('zh')) ? 'zh' : 'en';

    const result: InitializeResult = {
        capabilities: {
            textDocumentSync: TextDocumentSyncKind.Incremental,
            completionProvider: {
                resolveProvider: true
            },
            hoverProvider: true,
            definitionProvider: true,
            documentSymbolProvider: true,
            semanticTokensProvider: {
                legend: {
                    // 分类越细，编辑器主题给的颜色越分明（「各有各的颜色」）。
                    // 索引即 SEM_TOKEN_* 常量，改动需与 tokenTypeFor* 保持一致。
                    tokenTypes: [
                        'keyword',      // 0  控制流 / 声明等通用关键字
                        'type',         // 1  经典类型 int32 / double ...
                        'function',     // 2  内建函数 rvdl_* / qchain_* ...
                        'variable',     // 3  普通标识符
                        'number',       // 4  数字字面量
                        'string',       // 5  字符串字面量
                        'operator',     // 6  运算符
                        'class',        // 7  量子类型 Qubit / QObject / QModel ...
                        'macro',        // 8  morph 态射宏
                        'method',       // 9  量子门 h / x / cnot / rz ...
                        'decorator',    // 10 注解标签 @layer / @[gate] ...
                        'constant',     // 11 常量 true / false / null
                        'enum'          // 12 flavor 味成员
                    ],
                    tokenModifiers: []
                },
                full: true
            }
        }
    };
    return result;
});

connection.onNotification('ravendrel/runCode', async (params: { uri: string }) => {
    const document = documents.get(params.uri);
    if (document) {
        await compileAndExecute(document);
    }
});

connection.onNotification('ravendrel/compileCode', async (params: { uri: string; arch?: string }) => {
    const document = documents.get(params.uri);
    if (document) {
        await compileToBinary(document, params.arch ?? 'x64');
    }
});

connection.onNotification('ravendrel/buildCode', async (params: { uri: string }) => {
    const document = documents.get(params.uri);
    if (document) {
        await buildIR(document);
    }
});

// 迁移现有量子语言源码（OpenQASM/Q#/Quil/Silq）为 .rvdl：右键菜单触发。
// 直接读文件系统（源语言非 ravendrel 文档，不经 documents），迁移结果写到同名 .rvdl。
connection.onNotification('ravendrel/migrateCode', async (params: { fsPath: string }) => {
    try {
        const src = fs.readFileSync(params.fsPath, 'utf-8');
        const lang = detectLang(src);
        if (!lang) {
            connection.sendNotification('ravendrel/showConsole');
            connection.sendNotification('ravendrel/printConsole', '[Ravendrel Migrate] Cannot detect source language (expect OpenQASM/Q#/Quil/Silq).\n');
            return;
        }
        const qkSrc = migrate(src, lang);
        const outPath = params.fsPath.replace(/\.[^.]+$/, '.rvdl');
        fs.writeFileSync(outPath, qkSrc, 'utf-8');
        connection.sendNotification('ravendrel/showConsole');
        connection.sendNotification('ravendrel/printConsole', `[Ravendrel Migrate] ${path.basename(params.fsPath)} (${lang}) -> ${outPath}\n`);
    } catch (e: any) {
        connection.sendNotification('ravendrel/showConsole');
        connection.sendNotification('ravendrel/printConsole', `[Ravendrel Migrate] ${e.message}\n`);
    }
});

// ─── 一键构建 Android APK ────────────────────────────────────────────────
// 复用 CLI `rvdl build apk` 的编排脚本 scripts/build-apk.sh（AOT 交叉编译 + Gradle 打包）。
// 该脚本属于源码树文件，不在 vsix 内，所以按 RVDL_ROOT → 工作区根 → cwd/自身 向上查找。
function locateBuildApkScript(workspaceRoot?: string): string {
    const candidates: string[] = [];
    if (process.env.RVDL_ROOT) candidates.push(process.env.RVDL_ROOT);
    if (workspaceRoot) candidates.push(workspaceRoot);
    let dir = process.cwd();
    for (let i = 0; i < 6; i++) { candidates.push(dir); dir = path.dirname(dir); }
    dir = __dirname;
    for (let i = 0; i < 6; i++) { candidates.push(dir); dir = path.dirname(dir); }
    for (const root of candidates) {
        const p = path.join(root, 'scripts', 'build-apk.sh');
        if (fs.existsSync(p)) return p;
    }
    return '';
}

/** file:// URI → 本地路径（含 Windows 盘符与百分号转义处理） */
function uriToFsPath(uri: string): string {
    let p = uri.replace(/^file:\/\//, '');
    if (/^\/[a-zA-Z]:/.test(p)) p = p.slice(1);
    return decodeURIComponent(p);
}

/**
 * 选择可用的 bash。
 * Windows 上 PATH 里的 `bash` 常指向 %LOCALAPPDATA%\Microsoft\WindowsApps\bash.exe
 * ——那是 WSL 启动器，收到 `D:\...` 这类 Windows 路径无法解析；Git Bash 则可以直接用，
 * 因此优先探测 Git Bash（可用 RVDL_BASH 覆盖）。
 */
function resolveBash(): string {
    const explicit = process.env.RVDL_BASH;
    if (explicit && fs.existsSync(explicit)) return explicit;
    if (process.platform === 'win32') {
        for (const p of ['C:\\Program Files\\Git\\bin\\bash.exe', 'C:\\Program Files (x86)\\Git\\bin\\bash.exe']) {
            if (fs.existsSync(p)) return p;
        }
    }
    return 'bash';
}

connection.onNotification('ravendrel/buildApk', (params: { uri: string; release?: boolean; workspaceRoot?: string }) => {
    const fsPath = uriToFsPath(params.uri);
    connection.sendNotification('ravendrel/showConsole');

    if (!fs.existsSync(fsPath)) {
        connection.sendNotification('ravendrel/printConsole', `[Ravendrel APK] 文件不存在：${fsPath}\n`);
        return;
    }
    const script = locateBuildApkScript(params.workspaceRoot);
    if (!script) {
        connection.sendNotification('ravendrel/printConsole',
            '[Ravendrel APK] 未找到 scripts/build-apk.sh。\n'
            + '           请在 Ravendrel 源码树内打开工作区，或设置环境变量 RVDL_ROOT 指向源码根目录。\n');
        return;
    }

    const root = path.resolve(path.dirname(script), '..');
    const args = [script, fsPath];
    if (params.release) args.push('--release');

    connection.sendNotification('ravendrel/printConsole',
        `[Ravendrel APK] ${params.release ? 'release' : 'debug'} 构建开始：${path.basename(fsPath)}\n`
        + `           脚本：${script}\n`
        + '           需要 Android NDK（aarch64-linux-android*-clang++）与 bash；'
        + '首次构建 Gradle 可能耗时较久。\n');

    const bash = resolveBash();
    connection.sendNotification('ravendrel/printConsole', `           使用 shell：${bash}\n`);

    const t0 = Date.now();
    const child = spawn(bash, args, { cwd: root });
    child.stdout?.on('data', (d: Buffer) => connection.sendNotification('ravendrel/printConsole', d.toString()));
    child.stderr?.on('data', (d: Buffer) => connection.sendNotification('ravendrel/printConsole', d.toString()));
    child.on('error', (e: Error) => {
        connection.sendNotification('ravendrel/printConsole',
            `\n[Ravendrel APK] 启动失败：${e.message}\n`
            + '           Windows 下需要 bash（安装 Git Bash 并加入 PATH），或改用 WSL 执行。\n');
        connection.sendNotification('ravendrel/performance', {
            action: 'apk', compileMs: Date.now() - t0, execMs: 0, backend: 'gradle', status: 'error', at: Date.now(),
        });
    });
    child.on('close', (code: number | null) => {
        const ms = Date.now() - t0;
        connection.sendNotification('ravendrel/printConsole', code === 0
            ? `\n[Ravendrel APK] 构建完成 ✓（${(ms / 1000).toFixed(1)}s）产物见 scripts/build-apk.sh 输出目录。\n`
            : `\n[Ravendrel APK] 构建失败（exit ${code}），请查看上方日志。\n`);
        connection.sendNotification('ravendrel/performance', {
            action: 'apk', compileMs: ms, execMs: 0, backend: 'gradle',
            status: code === 0 ? 'done' : 'error', at: Date.now(),
        });
    });
});

connection.onCompletion(
    (params): CompletionItem[] => {
        // ── 取光标前的标识符前缀（输入 "all" → "all"），据此智能过滤 ──
        let prefix = '';
        let before = '';
        let inLayerTag = false;
        const doc = documents.get(params.textDocument.uri);
        if (doc) {
            before = doc.getText().slice(0, doc.offsetAt(params.position));
            const m = /[A-Za-z_][\w]*$/.exec(before);
            if (m) prefix = m[0];
            inLayerTag = insideLayerTagKeys(before);
        }

        const items: CompletionItem[] = [];
        const add = (label: string, kind: CompletionItemKind, detail: string, insertText?: string) => {
            items.push({ label, kind, detail, insertText: insertText ?? label });
        };
        for (const kw of CONTROL_KEYWORDS) add(kw, CompletionItemKind.Keyword, 'Control-flow keyword');
        for (const kw of OTHER_KEYWORDS) add(kw, CompletionItemKind.Keyword, 'Declaration keyword');
        for (const t of TYPE_KEYWORDS) add(t, CompletionItemKind.Class, 'Type');
        for (const fn of BUILTIN_FUNCTIONS) add(fn, CompletionItemKind.Function, 'Built-in function', `${fn}(`);
        for (const g of QUANTUM_GATE_NAMES) add(g, CompletionItemKind.Function, 'Quantum gate', `${g}(`);
        for (const ann of ANNOTATION_KEYWORDS) add(ann, CompletionItemKind.Property, 'Annotation tag');

        // ── @layer(...) 键位：只给键 ──
        // 该上下文在语法上只能接受键名，此时把关键字/内建函数一并弹出是纯噪声。
        // 补全条目直接取自 LAYER_KEY_DOCS 的键序（与悬停同一张表，
        // 避免「补得出来、看不懂」或「看得懂、写不出」的单侧漂移）。
        if (inLayerTag) {
            const keyItems: CompletionItem[] = LAYER_TAG_KEY_NAMES.map(k => ({
                label: k,
                kind: CompletionItemKind.Property,
                detail: LAYER_KEY_DOCS[k]?.purpose ?? '@layer key',
                insertText: LAYER_TAG_INSERTS[k] ?? `${k}=`,
            }));
            if (!prefix) return keyItems;
            const low = prefix.toLowerCase();
            const rank = (l: string) => (l === low ? 0 : l.startsWith(low) ? 1 : 2);
            return keyItems
                .filter(i => i.label.toLowerCase().includes(low))
                .sort((a, b) => rank(a.label) - rank(b.label) || a.label.length - b.label.length);
        }

        // 无前缀时限量返回，避免一次弹出数百项造成卡顿
        if (!prefix) return items.slice(0, 200);

        // 过滤 + 相关度排序：精确匹配 > 前缀匹配 > 子串匹配（同档按长度，短者优先）
        const lower = prefix.toLowerCase();
        const rank = (label: string): number => {
            const l = label.toLowerCase();
            if (l === lower) return 0;
            if (l.startsWith(lower)) return 1;
            return 2;
        };
        return items
            .filter(i => i.label.toLowerCase().includes(lower))
            .sort((a, b) => rank(a.label) - rank(b.label) || a.label.length - b.label.length);
    }
);

connection.onCompletionResolve(
    (item: CompletionItem): CompletionItem => {
        return item;
    }
);

// ─── 语义高亮（semantic tokens） ──────────
connection.onRequest(SemanticTokensRequest.type, (params: SemanticTokensParams) => {
    const document = documents.get(params.textDocument.uri);
    if (!document) return { data: [] };

    const builder = new SemanticTokensBuilder();
    const lexer = new Lexer(document.getText());

    // 语义 token 分类索引（须与 capabilities.semanticTokensProvider.legend 同序）
    const SEM = {
        KEYWORD: 0, TYPE: 1, FUNCTION: 2, VARIABLE: 3, NUMBER: 4, STRING: 5,
        OPERATOR: 6, CLASS: 7, MACRO: 8, METHOD: 9, DECORATOR: 10, CONSTANT: 11, ENUM: 12,
    } as const;

    const QUANTUM_TYPE_NAMES = new Set([
        'Qubit', 'QObject', 'QModel', 'QReservoir', 'QRegister', 'Result',
        'DiracState', 'BellState', 'QuantumRegister',
    ]);
    // 量子门在 lexer 中已不是关键字（语句/表达式位置按标识符处理），故需按名称识别
    // （QUANTUM_GATE_SET 为模块级定义，此处直接使用）
    // '@layer' → 'layer'、'@[gate]' → 'gate'
    const ANNOTATION_NAMES = new Set(
        ANNOTATION_KEYWORDS.map(a => a.replace(/^[@\[]/, '').replace(/\]$/, ''))
    );
    const CONSTANT_NAMES = new Set(['true', 'false', 'null']);

    const tokenTypeForKeyword = (value: string): number => {
        if (TYPE_KEYWORDS.has(value)) {
            return QUANTUM_TYPE_NAMES.has(value) ? SEM.CLASS : SEM.TYPE;
        }
        if (BUILTIN_FUNCTIONS.includes(value)) return SEM.FUNCTION;
        if (ANNOTATION_NAMES.has(value)) return SEM.DECORATOR;
        if (CONSTANT_NAMES.has(value)) return SEM.CONSTANT;
        if (value === 'morph') return SEM.MACRO;              // 态射宏声明
        if (value === 'flavor') return SEM.ENUM;              // 枚举（味）定义
        if (QUANTUM_GATE_SET.has(value)) return SEM.METHOD; // 量子门
        return SEM.KEYWORD;
    };

    /** 标识符 token 的分类：量子门 / morph 宏调用 / 注解名 各自着色 */
    const tokenTypeForIdentifier = (value: string): number => {
        if (QUANTUM_GATE_SET.has(value)) return SEM.METHOD;
        if (value === 'morph') return SEM.MACRO;
        if (ANNOTATION_NAMES.has(value)) return SEM.DECORATOR;
        if (value === 'true' || value === 'false') return SEM.CONSTANT; // null 是关键字
        return SEM.VARIABLE;
    };

    const operatorTokenTypes = new Set<TokenType>([
        TokenType.Equals, TokenType.EqualsEquals, TokenType.Plus, TokenType.Minus,
        TokenType.Star, TokenType.Slash, TokenType.Percent, TokenType.LessThan,
        TokenType.LessEqual, TokenType.GreaterThan, TokenType.GreaterEqual,
        TokenType.AndAnd, TokenType.OrOr, TokenType.NotEqual, TokenType.Bang,
        TokenType.ShiftLeft, TokenType.ShiftRight, TokenType.Ampersand, TokenType.Pipe,
        TokenType.Caret, TokenType.Tilde, TokenType.Arrow, TokenType.ColonColon
    ]);

    let token = lexer.getNextToken();
    while (token.type !== TokenType.EOF) {
        const length = token.length > 0 ? token.length : token.value.length || 1;
        let typeIndex: number = SEM.VARIABLE;
        if (token.type === TokenType.Keyword) {
            typeIndex = tokenTypeForKeyword(token.value);
        } else if (token.type === TokenType.Identifier) {
            typeIndex = tokenTypeForIdentifier(token.value);
        } else if (token.type === TokenType.Number) {
            typeIndex = SEM.NUMBER;
        } else if (token.type === TokenType.String) {
            typeIndex = SEM.STRING;
        } else if (operatorTokenTypes.has(token.type)) {
            typeIndex = SEM.OPERATOR;
        }

        builder.push(
            token.line - 1,
            token.column - 1,
            length,
            typeIndex,
            0
        );
        token = lexer.getNextToken();
    }

    return builder.build();
});

// ─── 悬停提示：关键字 / 类型 / 内置函数的详细说明（作用、语法、参数等）──────────
connection.onHover((params): Hover | null => {
    const document = documents.get(params.textDocument.uri);
    if (!document) return null;

    const text = document.getText();
    const offset = document.offsetAt(params.position);
    const lexer = new Lexer(text);

    // 悬停到注解标签（@layer / @[gate] / @[coherence] 等）时，按 '@name' / '@[name]' 键查找
    const annotationAt = (tokenStart: number): string | null => {
        if (tokenStart > 0 && text[tokenStart - 1] === '@') return '@';
        if (tokenStart > 1 && text[tokenStart - 1] === '[' && text[tokenStart - 2] === '@') return '@[';
        return null;
    };

    let token = lexer.getNextToken();
    while (token.type !== TokenType.EOF) {
        const start = document.offsetAt({ line: token.line - 1, character: token.column - 1 });
        const end = start + (token.length > 0 ? token.length : token.value.length || 1);
        if (offset >= start && offset <= end) {
            let content: string;
            if (token.type === TokenType.Keyword) {
                // @layer 键位上的可视键名优先于同名通用关键字（上下文更具体）
                const lk = enclosingLayerTag(text, start) !== null ? layerKeyDoc(token.value) : undefined;
                if (lk) {
                    content = renderHover(t(HOVER_TEXT.kind.layerKey), token.value, lk, hoverLang);
                } else if (TYPE_DOCS[token.value]) {
                    content = renderHover(t(HOVER_TEXT.kind.type), token.value, TYPE_DOCS[token.value], hoverLang);
                } else if (FUNCTION_DOCS[token.value]) {
                    content = renderHover(t(HOVER_TEXT.kind.fn), token.value, FUNCTION_DOCS[token.value], hoverLang);
                } else if (KEYWORD_DOCS[token.value]) {
                    content = renderHover(t(HOVER_TEXT.kind.keyword), token.value, KEYWORD_DOCS[token.value], hoverLang);
                } else {
                    content = `**${t(HOVER_TEXT.fallback.keyword)}** \`${token.value}\``;
                }
            } else if (token.type === TokenType.Identifier) {
                const annPrefix = annotationAt(start);
                const lk = enclosingLayerTag(text, start) !== null ? layerKeyDoc(token.value) : undefined;
                if (annPrefix) {
                    const key = annPrefix + token.value + (annPrefix === '@[' ? ']' : '');
                    if (ANNOTATION_DOCS[key]) {
                        content = renderHover(t(HOVER_TEXT.kind.annotation), key, ANNOTATION_DOCS[key], hoverLang);
                    } else {
                        content = `**${t(HOVER_TEXT.fallback.annotation)}** \`${key}\``;
                    }
                } else if (lk) {
                    content = renderHover(t(HOVER_TEXT.kind.layerKey), token.value, lk, hoverLang);
                } else if (FUNCTION_DOCS[token.value]) {
                    // 门函数（h / x / rz / cnot 等）在词法上属标识符，仍展示内置函数说明
                    content = renderHover(t(HOVER_TEXT.kind.fn), token.value, FUNCTION_DOCS[token.value], hoverLang);
                } else if (TYPE_DOCS[token.value]) {
                    // 部分类型名（如 void）在词法上属标识符，仍展示类型说明
                    content = renderHover(t(HOVER_TEXT.kind.type), token.value, TYPE_DOCS[token.value], hoverLang);
                } else {
                    content = `**${t(HOVER_TEXT.fallback.identifier)}** \`${token.value}\``;
                }
            } else if (token.type === TokenType.Number) {
                // @layer 枚举键的取值：`shuttle_strategy=3` 悬停到 3 时说明 3 是什么。
                // 这些取值本身是魔数，不悬停就只能去翻文档 —— 而这正是遗漏点。
                const key = enclosingLayerTag(text, start) !== null
                    ? layerKeyBefore(text, start) : null;
                const vd = key ? layerEnumValueDoc(key, String(Number(token.value))) : undefined;
                if (vd) {
                    content = `**\`${key} = ${token.value}\`** — ${t(HOVER_TEXT.kind.layerEnum)}\n\n${t(vd)}`;
                } else {
                    content = `**${t(HOVER_TEXT.fallback.number)}** \`${token.value}\``;
                }
            } else if (token.type === TokenType.String) {
                content = `**${t(HOVER_TEXT.fallback.string)}** \`${token.value}\``;
            } else {
                return null;
            }
            return { contents: { kind: MarkupKind.Markdown, value: content } };
        }
        token = lexer.getNextToken();
    }
    return null;
});

// ─── 文档大纲：函数 / 变量 / 模块声明 ──────────
connection.onDocumentSymbol((params): DocumentSymbol[] => {
    const document = documents.get(params.textDocument.uri);
    if (!document) return [];

    const symbols: DocumentSymbol[] = [];
    try {
        const lexer = new Lexer(document.getText());
        const parser = new Parser(lexer);
        const ast = parser.parse();

        const rangeOf = (node: any): Range => {
            const startLine = (node.line ?? 1) - 1;
            const startChar = (node.column ?? 1) - 1;
            const endLine = startLine;
            const endChar = startChar + (node.length ?? node.name?.length ?? 1);
            return Range.create(Position.create(startLine, startChar), Position.create(endLine, endChar));
        };

        const walk = (nodes: any[]) => {
            for (const node of nodes) {
                if (!node) continue;
                if (node.type === 'FunctionDeclaration') {
                    symbols.push({
                        name: node.name,
                        kind: SymbolKind.Function,
                        range: rangeOf(node),
                        selectionRange: rangeOf(node)
                    });
                    walk(node.body);
                } else if (node.type === 'VariableDeclaration') {
                    symbols.push({
                        name: node.identifier,
                        kind: SymbolKind.Variable,
                        range: rangeOf(node),
                        selectionRange: rangeOf(node)
                    });
                } else if (node.type === 'VariableDeclarationList') {
                    for (const d of node.declarations) {
                        symbols.push({
                            name: d.identifier,
                            kind: SymbolKind.Variable,
                            range: rangeOf(d),
                            selectionRange: rangeOf(d)
                        });
                    }
                } else if (node.type === 'FormDecl') {
                    symbols.push({
                        name: node.name,
                        kind: SymbolKind.Class,
                        range: rangeOf(node),
                        selectionRange: rangeOf(node)
                    });
                } else if (node.type === 'TraitDecl') {
                    symbols.push({
                        name: node.name,
                        kind: SymbolKind.Interface,
                        range: rangeOf(node),
                        selectionRange: rangeOf(node)
                    });
                } else if (node.type === 'ModuleDecl') {
                    symbols.push({
                        name: node.name,
                        kind: SymbolKind.Namespace,
                        range: rangeOf(node),
                        selectionRange: rangeOf(node)
                    });
                    walk(node.body);
                } else if (node.type === 'IfStatement') {
                    walk(node.consequent);
                    if (node.alternate) walk(node.alternate);
                } else if (node.type === 'WhileStatement' || node.type === 'SpinStatement') {
                    walk(node.body);
                } else if (node.type === 'ForStatement') {
                    walk(node.body);
                }
            }
        };
        walk(ast.body);
    } catch {
        // 解析失败时返回空大纲，不打断编辑器
    }
    return symbols;
});

// ─── 定义跳转：查找函数 / 变量的定义位置 ──────────
connection.onDefinition((params): Location[] => {
    const document = documents.get(params.textDocument.uri);
    if (!document) return [];

    const results: Location[] = [];
    try {
        const text = document.getText();
        const offset = document.offsetAt(params.position);
        const lexer = new Lexer(text);
        let token = lexer.getNextToken();
        let targetName = '';
        while (token.type !== TokenType.EOF) {
            const start = document.offsetAt({ line: token.line - 1, character: token.column - 1 });
            const end = start + (token.length > 0 ? token.length : token.value.length || 1);
            if (offset >= start && offset <= end && token.type === TokenType.Identifier) {
                targetName = token.value;
                break;
            }
            token = lexer.getNextToken();
        }

        if (!targetName) return [];

        const parser = new Parser(new Lexer(text));
        const ast = parser.parse();

        const locationOf = (node: any): Location => ({
            uri: params.textDocument.uri,
            range: Range.create(
                Position.create((node.line ?? 1) - 1, (node.column ?? 1) - 1),
                Position.create((node.line ?? 1) - 1, (node.column ?? 1) - 1 + (node.length ?? node.name?.length ?? 1))
            )
        });

        const search = (nodes: any[]) => {
            for (const node of nodes) {
                if (!node) continue;
                if (node.type === 'FunctionDeclaration' && node.name === targetName) {
                    results.push(locationOf(node));
                }
                if (node.type === 'VariableDeclaration' && node.identifier === targetName) {
                    results.push(locationOf(node));
                }
                if (node.type === 'VariableDeclarationList') {
                    for (const d of node.declarations) {
                        if (d.identifier === targetName) results.push(locationOf(d));
                    }
                }
                if (node.type === 'FormDecl' && node.name === targetName) {
                    results.push(locationOf(node));
                }
                if (node.type === 'TraitDecl' && node.name === targetName) {
                    results.push(locationOf(node));
                }
                if (node.body && Array.isArray(node.body)) search(node.body);
                if (node.consequent) search(node.consequent);
                if (node.alternate) search(node.alternate);
            }
        };
        search(ast.body);
    } catch {
        // 解析失败时返回空
    }
    return results;
});

documents.onDidChangeContent(change => {
    validateTextDocument(change.document);
});

documents.listen(connection);
connection.listen();

async function validateTextDocument(textDocument: TextDocument): Promise<void> {
    const text = textDocument.getText();
    const diagnostics: Diagnostic[] = [];

    try {
        const lexer = new Lexer(text);
        const parser = new Parser(lexer);
        const ast = parser.parse();

        const analyzer = new SemanticAnalyzer();
        analyzer.analyze(ast);

        for (const err of analyzer.errors) {
            diagnostics.push({
                severity: DiagnosticSeverity.Error,
                range: {
                    start: { line: err.line - 1, character: err.column - 1 },
                    end: { line: err.line - 1, character: err.column - 1 + err.length }
                },
                message: err.message,
                source: 'Ravendrel Semantic Analyzer'
            });
        }

        // 警告（如 W-SHU001）：显示为 Warning 而非 Error —— 它表示「这里会发生
        // 文档化的降级」，不阻断执行。误报为 Error 会让降级路径被 `errors.length > 0`
        // 挡死，用户看到的告警与文档承诺的行为相反。
        for (const warn of analyzer.warnings) {
            diagnostics.push({
                severity: DiagnosticSeverity.Warning,
                range: {
                    start: { line: warn.line - 1, character: warn.column - 1 },
                    end: { line: warn.line - 1, character: warn.column - 1 + warn.length }
                },
                message: warn.message,
                source: 'Ravendrel Semantic Analyzer'
            });
        }

        if (analyzer.errors.length === 0) {
            const vcGen = new VCGenerator();
            const obligations = vcGen.generate(ast);
            for (const ob of obligations) {
                diagnostics.push({
                    severity: DiagnosticSeverity.Information,
                    range: {
                        start: { line: 0, character: 0 },
                        end: { line: 0, character: 1 }
                    },
                    message: `Contract obligation '${ob.id}' awaiting static verification. Run 'rvdl verify' to prove or refute.`,
                    source: 'Ravendrel Verifier'
                });
            }
        }
    } catch (error: any) {
        let line = 0;
        let col = 0;
        let len = 10;

        const match = error.message.match(/line (\d+), col (\d+)/);
        if (match) {
            line = Math.max(0, parseInt(match[1]) - 1);
            col = Math.max(0, parseInt(match[2]) - 1);
            len = 1;
        }

        const diagnostic: Diagnostic = {
            severity: DiagnosticSeverity.Error,
            range: {
                start: { line: line, character: col },
                end: { line: line, character: col + len }
            },
            message: error.message,
            source: 'Ravendrel Compiler'
        };

        diagnostics.push(diagnostic);
    }

    connection.sendDiagnostics({ uri: textDocument.uri, diagnostics });
}

async function compileAndExecute(textDocument: TextDocument): Promise<void> {
    const text = textDocument.getText();

    try {
        const t0 = Date.now();
        const lexer = new Lexer(text);
        const parser = new Parser(lexer);
        const ast = parser.parse();
        const analyzer = new SemanticAnalyzer();
        analyzer.analyze(ast);

        if (analyzer.errors.length > 0) {
            connection.sendNotification('ravendrel/showConsole');
            connection.sendNotification('ravendrel/printConsole', "[Ravendrel] Execution aborted due to semantic errors.\n");
            connection.sendNotification('ravendrel/performance', { action: 'run', compileMs: 0, execMs: 0, backend: '—', status: 'semantic-error', at: Date.now() });
            return;
        }

        // 警告不阻断执行（W-SHU001 等降级告警照常运行），但必须让用户看见
        for (const warn of analyzer.warnings) {
            connection.sendNotification('ravendrel/printConsole', `[Ravendrel Warning] Line ${warn.line}: ${warn.message}\n`);
        }

        const irGen = new IRGenerator();
        const llvmIR = irGen.generate(ast);
        // 多维标签函数（@layer）走拓扑入口 rvdl_topology_entry；否则脚本模式 rvdl_main
        const hasLayerFns = ast.body.some(n => (n as any).type === 'FunctionDeclaration' && (n as any).layer);
        const entryFunc = hasLayerFns ? 'rvdl_topology_entry' : 'rvdl_main';
        const compileMs = Date.now() - t0;

        connection.sendNotification('ravendrel/showConsole');
        connection.sendNotification('ravendrel/clearConsole');
        connection.sendNotification('ravendrel/printConsole', `[Ravendrel JIT] Compiling target: ${textDocument.uri}\n`);
        connection.sendNotification('ravendrel/printConsole', `[Ravendrel JIT] Connecting to daemon (port ${DAEMON_PORT})...\n`);
        connection.sendNotification('ravendrel/printConsole', `--------------------------------------------------------\n`);

        // 执行走 daemon：用户代码量子操作作用在 daemon 共享 QVM，侧边栏可读快照
        const execStart = Date.now();
        await ensureDaemon();
        await daemonRequest([
            encodeFrame(Cmd.COMPILE, llvmIR),
            encodeFrame(Cmd.EXECUTE, `int32 ${entryFunc}`),
        ], (out) => connection.sendNotification('ravendrel/printConsole', out));
        const execMs = Date.now() - execStart;

        connection.sendNotification('ravendrel/printConsole', `--------------------------------------------------------\n`);
        connection.sendNotification('ravendrel/performance', { action: 'run', compileMs, execMs, backend: 'QVM', status: 'done', at: Date.now() });
    } catch (error: any) {
        connection.sendNotification('ravendrel/showConsole');
        connection.sendNotification('ravendrel/printConsole', `\n[Ravendrel System Error] ${error.message}\n`);
        connection.sendNotification('ravendrel/performance', { action: 'run', compileMs: 0, execMs: 0, backend: '—', status: 'error', at: Date.now() });
    }
}

// ─── 编译（AOT）：生成 IR + AOT 编译为指定架构原生二进制 ──────────
async function compileToBinary(textDocument: TextDocument, arch: string): Promise<void> {
    try {
        const t0 = Date.now();
        const text = textDocument.getText();
        const lexer = new Lexer(text);
        const parser = new Parser(lexer);
        const ast = parser.parse();
        const analyzer = new SemanticAnalyzer();
        analyzer.analyze(ast);
        if (analyzer.errors.length > 0) {
            connection.sendNotification('ravendrel/showConsole');
            connection.sendNotification('ravendrel/printConsole', "[Ravendrel] Compilation aborted due to semantic errors.\n");
            return;
        }
        for (const warn of analyzer.warnings) {
            connection.sendNotification('ravendrel/printConsole', `[Ravendrel Warning] Line ${warn.line}: ${warn.message}\n`);
        }
        const irGen = new IRGenerator();
        const llvmIR = irGen.generate(ast);
        const filePath = textDocument.uri.replace(/^file:\/\//, '');
        const baseName = path.basename(filePath).replace(/\.[^.]+$/, '');
        // 输出到源文件同目录（绝对路径；daemon 的 CWD 不可依赖）。
        const outputName = path.join(path.dirname(filePath), baseName);
        const compileMs = Date.now() - t0;

        connection.sendNotification('ravendrel/showConsole');
        connection.sendNotification('ravendrel/clearConsole');
        connection.sendNotification('ravendrel/printConsole', `[Ravendrel AOT] Compiling to native binary (${arch}): ${baseName}\n`);

        const isAndroid = arch === 'android' || arch === 'arm64-android';
        if (isAndroid) {
            connection.sendNotification('ravendrel/printConsole',
                '[Ravendrel AOT] Android 目标需要 NDK 交叉编译器 aarch64-linux-android{21,24,26}-clang++。\n'
                + '           可用环境变量指定：RVDL_ANDROID_CLANGXX（clang++ 路径）/ RVDL_ANDROID_SYSROOT（sysroot）'
                + ' / RVDL_ANDROID_RT_DIR（交叉编译出的 librvdl_rt.so 目录）。\n'
                + '           要打包成 APK 请用命令行：rvdl build apk <file.rvdl> [--release]\n');
        }

        await ensureDaemon();
        // Android：把扩展宿主进程的工具链环境变量随请求转发给 daemon
        // （daemon 可能在不同环境中启动，不能依赖其继承 RVDL_ANDROID_*）。
        const toolchainTokens: string[] = [];
        if (isAndroid) {
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
        // 帧载荷必须是 "<arch> <mode> <output_name> [k=v ...]\n<IR>"（见 runtime/src/main.cpp 的 CMD_AOT_COMPILE）。
        // 此前多写了一个 "compile" 前缀，后端会把 arch 解析成 "compile"、mode 解析成目标名，
        // 于是侧边栏选的目标实际不生效（始终按 host triple 编译）。
        await daemonRequest([
            encodeFrame(Cmd.AOT_COMPILE, `${[arch, '-e', outputName, ...toolchainTokens].join(' ')}\n${llvmIR}`),
        ], (out) => connection.sendNotification('ravendrel/printConsole', out));

        connection.sendNotification('ravendrel/performance', { action: 'compile', compileMs, execMs: 0, backend: '—', status: 'done', at: Date.now() });
    } catch (error: any) {
        connection.sendNotification('ravendrel/showConsole');
        connection.sendNotification('ravendrel/printConsole', `\n[Ravendrel System Error] ${error.message}\n`);
        connection.sendNotification('ravendrel/performance', { action: 'compile', compileMs: 0, execMs: 0, backend: '—', status: 'error', at: Date.now() });
    }
}

// ─── 构建（生成 IR）：生成 LLVM IR 保存到 .ll 文件 ──────────
async function buildIR(textDocument: TextDocument): Promise<void> {
    try {
        const text = textDocument.getText();
        const lexer = new Lexer(text);
        const parser = new Parser(lexer);
        const ast = parser.parse();
        const analyzer = new SemanticAnalyzer();
        analyzer.analyze(ast);
        if (analyzer.errors.length > 0) {
            connection.sendNotification('ravendrel/showConsole');
            connection.sendNotification('ravendrel/printConsole', "[Ravendrel] Build aborted due to semantic errors.\n");
            return;
        }
        for (const warn of analyzer.warnings) {
            connection.sendNotification('ravendrel/printConsole', `[Ravendrel Warning] Line ${warn.line}: ${warn.message}\n`);
        }
        const irGen = new IRGenerator();
        const llvmIR = irGen.generate(ast);
        const filePath = textDocument.uri.replace(/^file:\/\//, '');
        const outPath = filePath.replace(/\.[^.]+$/, '.ll');
        fs.writeFileSync(outPath, llvmIR, 'utf-8');

        connection.sendNotification('ravendrel/showConsole');
        connection.sendNotification('ravendrel/clearConsole');
        connection.sendNotification('ravendrel/printConsole', `[Ravendrel Build] LLVM IR generated.\n`);
        connection.sendNotification('ravendrel/printConsole', `[Ravendrel Build] Written to: ${outPath}\n`);
    } catch (error: any) {
        connection.sendNotification('ravendrel/showConsole');
        connection.sendNotification('ravendrel/printConsole', `\n[Ravendrel System Error] ${error.message}\n`);
    }
}