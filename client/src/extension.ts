import * as path from 'path';
import * as vscode from 'vscode';
import { ExtensionContext, window, OutputChannel} from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind
} from 'vscode-languageclient/node';
import {
    TargetProvider,
    ActionsProvider,
    PerformanceProvider,
    QuantumStateWebviewProvider,
    PerfSnapshot,
    CompileTarget,
    COMPILE_TARGETS
} from './ravendrelSidebar';

let client: LanguageClient;
let ravendrelConsole: OutputChannel;

export function activate(context: ExtensionContext) {
    ravendrelConsole = window.createOutputChannel('Ravendrel Console');
    context.subscriptions.push(ravendrelConsole);

    const serverModule = context.asAbsolutePath(
        path.join('server', 'out', 'server.js')
    );

    const serverOptions: ServerOptions = {
        run: { module: serverModule, transport: TransportKind.ipc },
        debug: {
            module: serverModule,
            transport: TransportKind.ipc,
            options: { execArgv: ['--nolazy', '--inspect=6009'] }
        }
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'ravendrel' }],
    };

    client = new LanguageClient(
        'ravendrelLanguageServer',
        'Ravendrel Language Server',
        serverOptions,
        clientOptions
    );

    client.start();

    client.onNotification('ravendrel/printConsole', (message: string) => {
        ravendrelConsole.append(message);
    });

    client.onNotification('ravendrel/showConsole', () => {
        ravendrelConsole.show(true); 
    });

    client.onNotification('ravendrel/clearConsole', () => {
        ravendrelConsole.clear();
    });

    const runCommand = vscode.commands.registerCommand('ravendrel.runScript', () => {
        const editor = vscode.window.activeTextEditor;
        if (editor) {
            const uri = editor.document.uri.toString();
            // 把自定义执行触发通知发给 server.ts
            client.sendNotification('ravendrel/runCode', { uri });
        }
    });

    const compileCommand = vscode.commands.registerCommand('ravendrel.compileScript', () => {
        const editor = vscode.window.activeTextEditor;
        if (editor) {
            const uri = editor.document.uri.toString();
            // 编译目标（x32/x64/arm64/android）从侧边栏读取，透传给 server
            client.sendNotification('ravendrel/compileCode', { uri, arch: targetProvider.getTarget() });
        }
    });

    const buildCommand = vscode.commands.registerCommand('ravendrel.buildScript', () => {
        const editor = vscode.window.activeTextEditor;
        if (editor) {
            const uri = editor.document.uri.toString();
            client.sendNotification('ravendrel/buildCode', { uri });
        }
    });

    // 迁移现有量子语言源码（OpenQASM/Q#/Quil/Silq）为 .rvdl：右键菜单触发。
    // 参数 uri 由 explorer/context 提供；编辑器右键时无参，回退到活动编辑器。
    const migrateCommand = vscode.commands.registerCommand('ravendrel.migrateScript', (uri?: vscode.Uri) => {
        let target = uri;
        if (!target) {
            const editor = vscode.window.activeTextEditor;
            target = editor?.document.uri;
        }
        if (!target) {
            vscode.window.showWarningMessage('Ravendrel: no file selected to migrate.');
            return;
        }
        client.sendNotification('ravendrel/migrateCode', { fsPath: target.fsPath });
    });

    // 一键构建 Android APK（AOT 交叉编译 + Gradle 打包）。
    // 侧边栏的动作项带 { release: false } 直接出 debug 包；命令面板调用时（无参）弹选择框。
    const buildApkCommand = vscode.commands.registerCommand('ravendrel.buildApk', async (opts?: { release?: boolean }) => {
        const editor = vscode.window.activeTextEditor;
        if (!editor || editor.document.languageId !== 'ravendrel') {
            vscode.window.showWarningMessage('Ravendrel: 请在打开的 .rvdl 文件上执行 Build APK。');
            return;
        }
        let release = opts?.release;
        if (release === undefined) {
            const pick = await vscode.window.showQuickPick(
                [
                    { label: 'Debug APK', description: '可调试，构建更快（默认）', release: false },
                    { label: 'Release APK', description: '需要签名配置，产物更小', release: true },
                ],
                { placeHolder: '选择 APK 构建类型' },
            );
            if (!pick) return;
            release = pick.release;
        }
        client.sendNotification('ravendrel/buildApk', {
            uri: editor.document.uri.toString(),
            release,
            workspaceRoot: vscode.workspace.workspaceFolders?.[0]?.uri.fsPath,
        });
    });

    // ─── 侧边栏视图 ──────────
    const targetProvider = new TargetProvider(context);
    const actionsProvider = new ActionsProvider();
    const performanceProvider = new PerformanceProvider(context);
    const quantumProvider = new QuantumStateWebviewProvider();

    context.subscriptions.push(
        vscode.window.registerTreeDataProvider('ravendrel.targetPlatform', targetProvider),
        vscode.window.registerTreeDataProvider('ravendrel.actions', actionsProvider),
        vscode.window.registerWebviewViewProvider('ravendrel.performance', performanceProvider, {
            webviewOptions: { retainContextWhenHidden: true },
        }),
        vscode.window.registerWebviewViewProvider('ravendrel.quantumState', quantumProvider, {
            webviewOptions: { retainContextWhenHidden: true },
        }),
        quantumProvider,
    );

    // 编译目标切换命令（侧边栏 TargetItem 触发）
    const setTargetCmd = vscode.commands.registerCommand('ravendrel.setCompileTarget', (t?: CompileTarget) => {
        // 从命令面板调用时没有实参，这里显式挡掉，避免把 undefined 写进 workspaceState
        if (!t || !COMPILE_TARGETS.includes(t)) {
            vscode.window.showWarningMessage('Ravendrel: 请在侧边栏「编译目标」中选择目标（x32 / x64 / arm64 / android）。');
            return;
        }
        targetProvider.setTarget(t);
        vscode.window.showInformationMessage(`Ravendrel 编译目标已切换为 ${t}`);
    });
    // 量子状态刷新命令（view/title 刷新按钮）
    const refreshQuantumCmd = vscode.commands.registerCommand('ravendrel.refreshQuantumState', () => {
        quantumProvider.refresh();
    });
    // 清空性能历史命令（view/title 清空按钮）
    const clearPerfCmd = vscode.commands.registerCommand('ravendrel.clearPerformance', () => {
        performanceProvider.clear();
        vscode.window.showInformationMessage('Ravendrel 性能历史已清空');
    });
    context.subscriptions.push(setTargetCmd, refreshQuantumCmd, clearPerfCmd);

    // 性能回传：server → client，更新性能监测面板
    client.onNotification('ravendrel/performance', (p: PerfSnapshot) => {
        performanceProvider.update(p);
    });

    context.subscriptions.push(runCommand);
    context.subscriptions.push(compileCommand);
    context.subscriptions.push(buildCommand);
    context.subscriptions.push(migrateCommand);
    context.subscriptions.push(buildApkCommand);
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    return client.stop();
}