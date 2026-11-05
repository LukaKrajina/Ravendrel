import { EditorView, basicSetup } from 'codemirror';
import { EditorState } from '@codemirror/state';
import { javascript } from '@codemirror/lang-javascript';
import { oneDark } from '@codemirror/theme-one-dark';
import { initI18n, setLocale, t, type Locale } from './i18n';

declare global {
    interface Window {
        SE: {
            runScript(code: string): Promise<{ ok: boolean; output: string }>;
            openFileDialog(): Promise<{ path: string; content: string } | { error: string } | null>;
            saveFileDialog(path: string | null, content: string): Promise<string | { error: string } | null>;
            setDirty(dirty: boolean): void;
            setLocale(locale: string): void;
            onMenu(callback: (action: string) => void): void;
            onRequestSave(callback: () => void): void;
            notifySaveDone(): void;
            confirmDiscard(): Promise<'save' | 'discard' | 'cancel'>;
        };
    }
}

let currentFile: string | null = null;
let isDirty = false;

const fileLabel = document.getElementById('status-file')!;
const diagLabel = document.getElementById('status-diag')!;

function updateFileLabel() {
    let name = currentFile ? currentFile.split(/[\\/]/).pop()! : t('status.untitled');
    if (isDirty) name += ' •';
    fileLabel.textContent = name;
}

function setDirty(d: boolean) {
    isDirty = d;
    updateFileLabel();
    if (window.SE.setDirty) window.SE.setDirty(d);
}

function setDiag(text: string, ok: boolean | null) {
    diagLabel.textContent = text;
    diagLabel.className = ok === null ? '' : ok ? 'ok' : 'error';
}

const shellEl = document.getElementById('shell')!;
function shellPrint(text: string) {
    shellEl.textContent += text;
    shellEl.scrollTop = shellEl.scrollHeight;
}
function shellClear() {
    shellEl.textContent = '';
}

const editorParent = document.getElementById('editor')!;
const startState = EditorState.create({
    doc: 'int32 rvdl_main() {\n    return 0;\n}\n',
    extensions: [
        basicSetup,
        javascript(),
        oneDark,
        EditorView.updateListener.of((update) => {
            if (update.docChanged) {
                setDirty(true);
                setDiag(t('status.modified'), null);
            }
        }),
    ],
});
const view = new EditorView({ state: startState, parent: editorParent });

function setEditorContent(text: string) {
    view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: text } });
}

document.getElementById('btn-open')!.addEventListener('click', async () => {
    const res = await window.SE.openFileDialog();
    if (!res) return;
    if ('error' in res) {
        setDiag(res.error, false);
        shellPrint(`[SE] ${res.error}\n`);
        return;
    }
    currentFile = res.path;
    setEditorContent(res.content);
    setDirty(false);
    setDiag(t('status.opened'), true);
    shellPrint(`[SE] ${t('shell.opened')} ${res.path}\n`);
});

async function doSave(): Promise<boolean> {
    const content = view.state.doc.toString();
    const saved = await window.SE.saveFileDialog(currentFile, content);
    if (saved && typeof saved === 'string') {
        currentFile = saved;
        setDirty(false);
        setDiag(t('status.saved'), true);
        shellPrint(`[SE] ${t('shell.saved')} ${saved}\n`);
        return true;
    } else if (saved && typeof saved === 'object') {
        setDiag((saved as { error: string }).error, false);
        shellPrint(`[SE] ${(saved as { error: string }).error}\n`);
    }
    return false;
}

document.getElementById('btn-save')!.addEventListener('click', () => {
    doSave();
});

document.getElementById('btn-run')!.addEventListener('click', async () => {
    shellClear();
    shellPrint(`[SE] ${t('shell.compiling')}\n`);
    setDiag(t('status.compiling'), null);
    const res = await window.SE.runScript(view.state.doc.toString());
    shellPrint(res.output + '\n');
    if (res.ok) {
        setDiag(t('status.success'), true);
    } else {
        setDiag(t('status.fail'), false);
    }
});

document.getElementById('btn-clear')!.addEventListener('click', () => {
    shellClear();
});

const langSelect = document.getElementById('lang-select') as HTMLSelectElement;
langSelect.addEventListener('change', () => {
    setLocale(langSelect.value as Locale);
    updateFileLabel();
});

const locale = initI18n();
langSelect.value = locale;
updateFileLabel();

// 快捷键
document.addEventListener('keydown', (e) => {
    if (!(e.ctrlKey || e.metaKey)) return;
    const key = e.key.toLowerCase();
    if (key === 's') {  //Ctrl/Cmd+S 保存
        e.preventDefault();
        (document.getElementById('btn-save') as HTMLButtonElement).click();
    } else if (key === 'o') {   //Ctrl/Cmd+O 打开
        e.preventDefault();
        (document.getElementById('btn-open') as HTMLButtonElement).click();
    } else if (key === 'enter') {   //Ctrl/Cmd+Enter 运行
        e.preventDefault();
        (document.getElementById('btn-run') as HTMLButtonElement).click();
    }
});

async function startNew() {
    if (isDirty) {
        const choice = await window.SE.confirmDiscard();
        if (choice === 'cancel') return;
        if (choice === 'save') {
            const ok = await doSave();
            if (!ok) return; // 保存失败或取消保存对话框，则不新建
        }
    }
    currentFile = null;
    setEditorContent('');
    setDirty(false);
    setDiag('', null);
    shellClear();
}

// 菜单栏动作（File / View）
window.SE.onMenu((action) => {
    if (action === 'new') startNew();
    else if (action === 'open') (document.getElementById('btn-open') as HTMLButtonElement).click();
    else if (action === 'save') (document.getElementById('btn-save') as HTMLButtonElement).click();
    else if (action === 'run') (document.getElementById('btn-run') as HTMLButtonElement).click();
});

// 关闭前「保存后关闭」请求
window.SE.onRequestSave(async () => {
    await doSave();
    window.SE.notifySaveDone();
});