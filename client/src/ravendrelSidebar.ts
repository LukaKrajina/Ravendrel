// ============================================================================
// ravendrelSidebar.ts —— Ravendrel 侧边栏
//
// 四个视图：
//   1. 编译目标（TreeView）—— x32 / x64 / arm64 / android 切换，存入 workspaceState
//   2. 操作（TreeView）—— 运行 / 编译 / 构建 / 迁移快捷按钮
//   3. 性能监测（TreeView）—— 最近操作的编译/执行耗时、后端、状态
//   4. 量子对象与比特（WebviewView）—— daemon 快照：Bloch 球 + 态矢量概率柱状图
// ============================================================================

import * as vscode from 'vscode';
import * as net from 'net';

// daemon 端口。**单一来源在 server/src/protocol.ts**（DAEMON_PORT）——
// 客户端因构建布局（tsconfig rootDir 限制）无法 import server 源码，
// 故此处保留本地常量；改端口时以 protocol.ts 为准同步这一行。
const DAEMON_PORT = 50052;
const GET_SNAPSHOT_CMD = 0x09;

// ─── 编译目标 ────────────────────────────────────────────────────────────────

export const COMPILE_TARGETS = ['x32', 'x64', 'arm64', 'android'] as const;
export type CompileTarget = (typeof COMPILE_TARGETS)[number];

/** 目标 → LLVM triple 语义提示（x64 额外标注默认值） */
const TARGET_HINT: Record<CompileTarget, string> = {
    x32: 'i686',
    x64: '默认 · x86_64',
    arm64: 'aarch64',
    android: 'aarch64-linux-android',
};

/** Android 需要 NDK 交叉编译器；缺失时给出可操作的环境变量提示 */
const ANDROID_TOOLTIP = [
    'Android 目标（aarch64-linux-android）',
    '需要 NDK 交叉编译器：aarch64-linux-android{21,24,26}-clang++',
    '可用环境变量指定：',
    '  RVDL_ANDROID_CLANGXX  NDK clang++ 绝对路径',
    '  RVDL_ANDROID_SYSROOT  NDK sysroot（bionic libc / libc++）',
    '  RVDL_ANDROID_RT_DIR   交叉编译出的 librvdl_rt.so 目录',
    '打包 APK 用命令行：rvdl build apk <file.rvdl> [--release]',
].join('\n');

class TargetItem extends vscode.TreeItem {
    constructor(readonly target: CompileTarget, selected: boolean) {
        super(target, vscode.TreeItemCollapsibleState.None);
        this.description = TARGET_HINT[target];
        this.iconPath = new vscode.ThemeIcon(selected ? 'check' : (target === 'android' ? 'device-mobile' : 'circle-outline'));
        this.tooltip = target === 'android' ? ANDROID_TOOLTIP : `编译目标：${target}（${TARGET_HINT[target]}）`;
        this.command = { command: 'ravendrel.setCompileTarget', title: 'Set Target', arguments: [target] };
    }
}

export class TargetProvider implements vscode.TreeDataProvider<TargetItem> {
    private _onDidChange = new vscode.EventEmitter<void>();
    readonly onDidChangeTreeData = this._onDidChange.event;

    constructor(private context: vscode.ExtensionContext) {}

    getTreeItem(e: TargetItem): TargetItem { return e; }
    getChildren(): TargetItem[] {
        const current = this.getTarget();
        return COMPILE_TARGETS.map(t => new TargetItem(t, t === current));
    }
    getTarget(): CompileTarget {
        return (this.context.workspaceState.get<CompileTarget>('ravendrel.compileTarget')) ?? 'x64';
    }
    setTarget(t: CompileTarget): void {
        this.context.workspaceState.update('ravendrel.compileTarget', t);
        this._onDidChange.fire();
    }
}

// ─── 操作 ────────────────────────────────────────────────────────────────────

class ActionItem extends vscode.TreeItem {
    constructor(label: string, icon: string, command: string, detail?: string, args?: unknown[]) {
        super(label, vscode.TreeItemCollapsibleState.None);
        this.iconPath = new vscode.ThemeIcon(icon);
        this.description = detail;
        this.command = { command, title: label, arguments: args };
    }
}

export class ActionsProvider implements vscode.TreeDataProvider<ActionItem> {
    private _onDidChange = new vscode.EventEmitter<void>();
    readonly onDidChangeTreeData = this._onDidChange.event;
    getTreeItem(e: ActionItem): ActionItem { return e; }
    getChildren(): ActionItem[] {
        return [
            new ActionItem('Run Script', 'play', 'ravendrel.runScript'),
            new ActionItem('Compile (AOT)', 'package', 'ravendrel.compileScript'),
            new ActionItem('Build (IR)', 'tools', 'ravendrel.buildScript'),
            // 一键打 Android APK：侧边栏点一下直接出 debug 包；
            // 从命令面板调用（无参）时会弹出 Debug/Release 选择。
            new ActionItem('Build APK (Android)', 'device-mobile', 'ravendrel.buildApk', 'debug', [{ release: false }]),
            new ActionItem('Migrate to rvdl', 'git-compare', 'ravendrel.migrateScript'),
        ];
    }
}

// ─── 性能监测 ────────────────────────────────────────────────────────────────

export interface PerfSnapshot {
    action: string;
    compileMs: number;
    execMs: number;
    backend: string;
    status: string;
    at: number;
}

/**
 * 性能监测（WebviewView）：展示最近一次操作摘要 + 执行历史耗时曲线图。
 * server 每次编译/执行通过 `ravendrel/performance` 通知回传，此处累积历史并绘图。
 */
export class PerformanceProvider implements vscode.WebviewViewProvider {
    private view: vscode.WebviewView | null = null;
    private history: PerfSnapshot[] = [];

    constructor(private context: vscode.ExtensionContext) {
        // 跨会话持久化：从 globalState 恢复执行历史
        this.history = context.globalState.get<PerfSnapshot[]>('ravendrel.perfHistory', []);
    }

    resolveWebviewView(webviewView: vscode.WebviewView): void {
        this.view = webviewView;
        webviewView.webview.options = { enableScripts: true };
        webviewView.webview.html = this.renderHtml();
        this.postHistory();
    }

    update(p: PerfSnapshot): void {
        this.history.push(p);
        if (this.history.length > 60) this.history.shift();
        this.context.globalState.update('ravendrel.perfHistory', this.history);
        this.postHistory();
    }

    clear(): void {
        this.history = [];
        this.context.globalState.update('ravendrel.perfHistory', []);
        this.postHistory();
    }

    private postHistory(): void {
        this.view?.webview.postMessage({ type: 'perf', history: this.history });
    }

    private renderHtml(): string {
        return `<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<style>
  body { font-family: var(--vscode-font-family); color: var(--vscode-foreground); padding: 8px 10px; }
  h3 { margin: 10px 0 4px; font-size: 12px; color: #c586c0; }
  .row { font-size: 12px; margin: 2px 0; color: var(--vscode-descriptionForeground); }
  svg { display: block; margin: 4px 0; }
  .legend span { margin-right: 12px; }
</style>
</head>
<body>
  <h3>最近操作</h3>
  <div id="summary"></div>
  <h3>耗时历史</h3>
  <div id="chart"></div>
<script>
const vscode = acquireVsCodeApi();
let history = [];
window.addEventListener('message', e => { if (e.data.type === 'perf') { history = e.data.history; render(); } });

function render() {
  const last = history[history.length - 1];
  document.getElementById('summary').innerHTML = last
    ? '<div class="row">' + last.action + ' · 编译 ' + last.compileMs + 'ms · 执行 ' + last.execMs + 'ms · ' + last.backend + ' · ' + last.status + '</div>'
    : '<div class="row">空闲 (idle)</div>';
  document.getElementById('chart').innerHTML = drawChart(history);
}

function drawChart(h) {
  if (h.length < 2) return '<div class="row">数据不足，执行几次后显示曲线</div>';
  const W = 320, H = 150, PAD = 22;
  const maxMs = Math.max(10, ...h.map(p => Math.max(p.compileMs, p.execMs)));
  const n = h.length;
  const x = i => PAD + (W - 2 * PAD) * i / (n - 1);
  const y = v => H - PAD - (H - 2 * PAD) * v / maxMs;
  const line = key => h.map((p, i) => x(i).toFixed(1) + ',' + y(p[key]).toFixed(1)).join(' ');
  let s = '<svg width="' + W + '" height="' + H + '">';
  s += '<line x1="' + PAD + '" y1="' + (H - PAD) + '" x2="' + (W - PAD) + '" y2="' + (H - PAD) + '" stroke="#888" stroke-width="1"/>';
  s += '<line x1="' + PAD + '" y1="' + PAD + '" x2="' + PAD + '" y2="' + (H - PAD) + '" stroke="#888" stroke-width="1"/>';
  s += '<polyline fill="none" stroke="#c586c0" stroke-width="1.5" points="' + line('compileMs') + '"/>';
  s += '<polyline fill="none" stroke="#569cd6" stroke-width="1.5" points="' + line('execMs') + '"/>';
  s += '</svg>';
  s += '<div class="row legend"><span style="color:#c586c0">■</span> 编译耗时 <span style="color:#569cd6">■</span> 执行耗时 <span>峰值 ' + maxMs.toFixed(0) + 'ms</span></div>';
  return s;
}
render();
</script>
</body>
</html>`;
    }
}

// ─── 量子状态 WebviewView（Bloch 球 + 概率柱状图）──────────────────────────

export interface BlochVector { x: number; y: number; z: number; }

export interface QuantumSnapshot {
    generation: number;
    numQubits: number;
    backend: string;
    amplitudes: [number, number][];      // [re, im]（实部, 虚部）
    /** 每个 qubit 的约化密度矩阵 Bloch 向量（多 qubit 系统逐比特约化） */
    bloch: BlochVector[];
    objects: { type: string; ids: number[]; theta?: number; phi?: number }[];
    measurements: { qubit: number; result: number }[];
}

export class QuantumStateWebviewProvider implements vscode.WebviewViewProvider {
    private view: vscode.WebviewView | null = null;
    private socket: net.Socket | null = null;
    private buf: Buffer = Buffer.alloc(0);
    private timer: NodeJS.Timeout | null = null;
    private connected = false;

    resolveWebviewView(webviewView: vscode.WebviewView): void {
        this.view = webviewView;
        webviewView.webview.options = { enableScripts: true };
        webviewView.webview.html = this.renderHtml();
        webviewView.webview.onDidReceiveMessage(msg => {
            if (msg.type === 'refresh') this.fetchSnapshot();
        });
        this.startPolling();
    }

    refresh(): void { this.fetchSnapshot(); }

    dispose(): void {
        if (this.timer) clearInterval(this.timer);
        this.socket?.destroy();
    }

    private startPolling(): void {
        if (this.timer) clearInterval(this.timer);
        this.timer = setInterval(() => this.fetchSnapshot(), 2000);
        this.fetchSnapshot();
    }

    private fetchSnapshot(): void {
        if (!this.view) return;
        if (!this.socket || this.socket.destroyed) this.connect();
        if (!this.socket || this.socket.destroyed) return;
        // [4字节大端长度][命令字节]
        const body = Buffer.from([GET_SNAPSHOT_CMD]);
        const header = Buffer.alloc(4);
        header.writeUInt32BE(body.length, 0);
        this.socket.write(Buffer.concat([header, body]));
    }

    private connect(): void {
        this.socket = net.createConnection({ port: DAEMON_PORT, host: 'localhost' });
        this.socket.on('connect', () => {
            this.connected = true;
            this.post({ type: 'connected' });
        });
        this.socket.on('data', (d: Buffer) => this.onData(d));
        this.socket.on('close', () => {
            this.connected = false;
            this.socket = null;
            this.post({ type: 'disconnected' });
        });
        this.socket.on('error', () => { /* daemon 未运行 */ });
    }

    private onData(d: Buffer): void {
        this.buf = Buffer.concat([this.buf, d]);
        while (this.buf.length >= 4) {
            const len = this.buf.readUInt32BE(0);
            if (this.buf.length < 4 + len) break;
            const payload = this.buf.subarray(4, 4 + len).toString('utf-8');
            this.buf = this.buf.subarray(4 + len);
            const snap = parseSnapshot(payload);
            if (snap) this.post({ type: 'snapshot', data: snap });
        }
    }

    private post(msg: unknown): void {
        this.view?.webview.postMessage(msg);
    }

    private renderHtml(): string {
        return `<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<style>
  :root { --rvdl: #c586c0; --fg: var(--vscode-foreground); --muted: var(--vscode-descriptionForeground); }
  body { font-family: var(--vscode-font-family); color: var(--fg); padding: 8px 10px; }
  h3 { margin: 10px 0 4px; font-size: 12px; color: var(--rvdl); }
  .row { font-size: 12px; margin: 2px 0; color: var(--muted); }
  .bar-wrap { background: var(--vscode-input-background); border-radius: 3px; height: 14px; margin: 2px 0; overflow: hidden; }
  .bar { height: 100%; background: linear-gradient(90deg, #c586c0, #569cd6); }
  .bar-label { font-size: 10px; margin: 0 4px; }
  svg { display: block; margin: 4px auto; }
  .chip { display: inline-block; background: var(--vscode-badge-background); color: var(--vscode-badge-foreground); border-radius: 8px; padding: 0 6px; margin: 1px; font-size: 11px; }
</style>
</head>
<body>
  <div id="status">未连接 daemon（执行 rvdl 代码将自动启动）</div>
  <h3>态矢量概率</h3>
  <div id="probs"></div>
  <h3>Bloch 球（约化密度矩阵）</h3>
  <div id="bloch"></div>
  <h3>量子对象</h3>
  <div id="objects"></div>
  <h3>测量历史</h3>
  <div id="meas"></div>
<script>
const vscode = acquireVsCodeApi();
window.addEventListener('message', e => {
  const m = e.data;
  if (m.type === 'connected') setStatus('已连接 daemon');
  else if (m.type === 'disconnected') setStatus('未连接 daemon（执行 rvdl 代码将自动启动）');
  else if (m.type === 'snapshot') render(m.data);
});
function setStatus(s) { document.getElementById('status').textContent = s; }

function render(s) {
  document.getElementById('status').textContent = '后端 ' + s.backend + ' · ' + s.numQubits + ' qubit';
  // 概率柱状图（≤8 qubit）
  const probs = document.getElementById('probs');
  if (s.amplitudes && s.numQubits <= 8) {
    probs.innerHTML = s.amplitudes.map((a, i) => {
      const p = a[0]*a[0] + a[1]*a[1];
      const idx = i.toString(2).padStart(Math.max(s.numQubits,1), '0');
      return '<div class="row">|' + idx + '⟩</div><div class="bar-wrap"><div class="bar" style="width:' + (p*100).toFixed(1) + '%"></div></div>';
    }).join('');
  } else {
    probs.innerHTML = s.numQubits > 8 ? '<div class="row">qubit 数过大，略去概率图</div>' : '<div class="row">无态矢量</div>';
  }
  // Bloch 球：每个 qubit 的约化密度矩阵 Bloch 向量（多 qubit 逐比特约化，3D 可旋转）
  blochVecs = (s.bloch && s.bloch.length) ? s.bloch
    : (s.numQubits === 1 && s.amplitudes && s.amplitudes.length === 2 ? [amplToBloch(s.amplitudes[0], s.amplitudes[1])] : []);
  renderBloch();
  // 量子对象
  document.getElementById('objects').innerHTML = s.objects.length
    ? s.objects.map(o => '<span class="chip">' + o.type + ' [' + o.ids.join(',') + ']</span>').join('')
    : '<div class="row">无</div>';
  // 测量历史
  document.getElementById('meas').innerHTML = s.measurements.length
    ? s.measurements.slice(-12).map(m => '<span class="chip">q' + m.qubit + '→' + m.result + '</span>').join('')
    : '<div class="row">无</div>';
}

// 单 qubit 振幅 [a,b] → Bloch 向量（θ=2arccos|a|，φ=arg(b)−arg(a)）
function amplToBloch(a, b) {
  const a0 = Math.hypot(a[0], a[1]);
  const theta = 2 * Math.acos(Math.min(1, a0));
  const phi = Math.atan2(b[1], b[0]) - Math.atan2(a[1], a[0]);
  return {
    x: Math.sin(theta) * Math.cos(phi),
    y: Math.sin(theta) * Math.sin(phi),
    z: Math.cos(theta),
  };
}

// ─── Bloch 球 3D 交互（鼠标拖动旋转，观察 y 分量）──────────
let blochVecs = [];
let yaw = 0.6, pitch = 0.35;   // 初始视角，让 y 分量部分可见
let dragging = false, lastX = 0, lastY = 0;

// 旋转 Bloch 向量：先绕 Y 轴转 yaw，再绕 X 轴转 pitch
function rot(v, ya, pi) {
  const cy = Math.cos(ya), sy = Math.sin(ya);
  const x1 = v.x * cy + v.z * sy;
  const z1 = -v.x * sy + v.z * cy;
  const y1 = v.y;
  const cp = Math.cos(pi), sp = Math.sin(pi);
  const y2 = y1 * cp - z1 * sp;
  const z2 = y1 * sp + z1 * cp;
  return { x: x1, y: y2, z: z2 };
}

function renderBloch() {
  const container = document.getElementById('bloch');
  if (!blochVecs.length) { container.innerHTML = '<div class="row">无态矢量</div>'; return; }
  container.innerHTML = '<div style="display:flex;flex-wrap:wrap;justify-content:center;gap:4px">' +
    blochVecs.map((_, i) => '<canvas id="blochc' + i + '" width="120" height="132" style="cursor:grab"></canvas>').join('') +
    '</div>' +
    '<div class="row">拖动旋转观察 y 分量 · x/z 为屏幕水平/垂直</div>';
  blochVecs.forEach((v, i) => drawBloch3D(document.getElementById('blochc' + i), v, i));
}

function drawBloch3D(canvas, v, label) {
  const ctx = canvas.getContext('2d');
  const cx = 60, cy = 62, R = 46;
  ctx.clearRect(0, 0, 120, 132);

  // 球体轮廓
  ctx.strokeStyle = '#888'; ctx.lineWidth = 1;
  ctx.beginPath(); ctx.arc(cx, cy, R, 0, 2 * Math.PI); ctx.stroke();

  // 赤道椭圆（pitch 决定投影短轴）
  const eqRx = R * Math.abs(Math.cos(pitch));
  ctx.beginPath(); ctx.ellipse(cx, cy, R, Math.max(2, eqRx), 0, 0, 2 * Math.PI); ctx.stroke();

  // 经线（绕 Y 的几条，体现 yaw）
  for (let a = 0; a < Math.PI; a += Math.PI / 4) {
    const sx = cx + R * Math.cos(a) * Math.cos(yaw);
    const top = cy - R * Math.sin(a);
    const bot = cy + R * Math.sin(a);
    ctx.beginPath(); ctx.moveTo(cx - sx + cx, top); ctx.lineTo(sx, bot); ctx.stroke();
  }

  // 状态向量（旋转 + 正交投影：屏幕 x = x'，屏幕 y = -z'，y' 用颜色冷暖表示）
  const r = rot(v, yaw, pitch);
  const px = cx + R * r.x;
  const py = cy - R * r.z;
  const purity = Math.hypot(v.x, v.y, v.z);
  const yComp = r.y; // -1..1，用于颜色冷暖
  const hue = yComp >= 0 ? '#569cd6' : '#d16969'; // 正 y 偏蓝，负 y 偏红
  const col = purity > 0.99 ? '#c586c0' : '#d7ba7d';
  ctx.strokeStyle = col; ctx.lineWidth = 2;
  ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(px, py); ctx.stroke();
  ctx.fillStyle = col;
  ctx.beginPath(); ctx.arc(px, py, 2.5, 0, 2 * Math.PI); ctx.fill();
  // y 分量指示条（右上角）
  ctx.fillStyle = hue;
  ctx.fillRect(4, 114, 24, 6);
  ctx.fillStyle = '#888'; ctx.font = '9px sans-serif';
  ctx.fillText('y' + (yComp >= 0 ? '+' : '−'), 30, 120);

  // 标签 + 纯度
  ctx.fillStyle = '#888';
  ctx.fillText('q' + label, 4, 10);
  ctx.fillText((purity * 100).toFixed(0) + '%', 82, 126);
}

// 鼠标拖动旋转（事件委托到 bloch 容器，避免 canvas 重建后丢失监听）
document.getElementById('bloch').addEventListener('mousedown', e => {
  if (e.target.tagName === 'CANVAS') { dragging = true; lastX = e.clientX; lastY = e.clientY; e.target.style.cursor = 'grabbing'; }
});
window.addEventListener('mousemove', e => {
  if (!dragging) return;
  yaw += (e.clientX - lastX) * 0.01;
  pitch = Math.max(-1.5, Math.min(1.5, pitch + (e.clientY - lastY) * 0.01));
  lastX = e.clientX; lastY = e.clientY;
  renderBloch();
});
window.addEventListener('mouseup', () => { dragging = false; });
</script>
</body>
</html>`;
    }
}

// ─── 快照解析（SNAPSHOT 行协议）─────────────────────────────────────────────

function parseSnapshot(text: string): QuantumSnapshot | null {
    if (!text.includes('SNAPSHOT')) return null;
    const snap: QuantumSnapshot = {
        generation: 0, numQubits: 0, backend: '',
        amplitudes: [], bloch: [], objects: [], measurements: [],
    };
    let lastObject: QuantumSnapshot['objects'][number] | null = null;
    for (const raw of text.split('\n')) {
        // 线格式首行为 "RESPONSE: SNAPSHOT <gen> <n>"（gui/protocol.hpp 序列化前缀），
        // 逐行剥离可选的 "RESPONSE: " 前缀后再分词，否则 generation/numQubits 解析失败。
        let line = raw.trim();
        if (line.startsWith('RESPONSE: ')) line = line.slice('RESPONSE: '.length).trim();
        if (!line) continue;
        const p = line.split(/\s+/);
        switch (p[0]) {
            case 'SNAPSHOT':
                snap.generation = Number(p[1]);
                snap.numQubits = Number(p[2]);
                break;
            case 'Q':
                snap.amplitudes.push([Number(p[1]), Number(p[2])]);
                break;
            case 'O':
                lastObject = { type: p[1], ids: p.slice(2).map(Number) };
                snap.objects.push(lastObject);
                break;
            case 'P':
                if (lastObject) { lastObject.theta = Number(p[1]); lastObject.phi = Number(p[2]); }
                break;
            case 'M':
                snap.measurements.push({ qubit: Number(p[1]), result: Number(p[2]) });
                break;
            case 'B':
                snap.backend = p.slice(1).join(' ');
                break;
        }
    }
    snap.bloch = computeBlochVectors(snap.numQubits, snap.amplitudes);
    return snap;
}

/**
 * 计算每个 qubit 的约化密度矩阵 Bloch 向量。
 * 对第 k 个 qubit：ρ_k = Tr_{其他}(|ψ⟩⟨ψ|)，Bloch 向量
 *   r_x = 2·Re(ρ_01)，r_y = 2·Im(ρ_01)，r_z = ρ_00 − ρ_11。
 * 其中 ρ_01 = Σ_{rest} ψ(0_k, rest)·ψ*(1_k, rest)。
 */
function computeBlochVectors(numQubits: number, amplitudes: [number, number][]): BlochVector[] {
    if (numQubits <= 0 || amplitudes.length !== (1 << numQubits)) return [];
    const N = 1 << numQubits;
    const vecs: BlochVector[] = [];
    for (let k = 0; k < numQubits; k++) {
        let rho00 = 0, rho11 = 0, rho01Re = 0, rho01Im = 0;
        for (let i = 0; i < N; i++) {
            if (i & (1 << k)) {
                rho11 += amplitudes[i][0] ** 2 + amplitudes[i][1] ** 2;
            } else {
                rho00 += amplitudes[i][0] ** 2 + amplitudes[i][1] ** 2;
                const j = i | (1 << k);
                const a = amplitudes[i], b = amplitudes[j];
                // ρ_01 += ψ(0)·conj(ψ(1)) = (a0+i·a1)(b0−i·b1)
                rho01Re += a[0] * b[0] + a[1] * b[1];
                rho01Im += a[1] * b[0] - a[0] * b[1];
            }
        }
        vecs.push({ x: 2 * rho01Re, y: 2 * rho01Im, z: rho00 - rho11 });
    }
    return vecs;
}