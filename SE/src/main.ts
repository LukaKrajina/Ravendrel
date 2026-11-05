import { app, BrowserWindow, ipcMain, dialog, Menu } from 'electron';
import * as path from 'path';
import * as fs from 'fs';

let win: BrowserWindow | null = null;

// ── 主进程文案（窗口标题 / 对话框标题）──
type Locale = 'en' | 'zh' | 'ru' | 'fr' | 'de';
const MAIN_STRINGS: Record<
    Locale,
    {
        windowTitle: string;
        dirtySuffix: string;
        openTitle: string;
        saveTitle: string;
        qkFilter: string;
        allFilter: string;
        daemonError: string;
        closeTitle: string;
        closeSave: string;
        closeDiscard: string;
        closeCancel: string;
    }
> = {
    en: {
        windowTitle: 'SE — rvdl Editor',
        dirtySuffix: ' (unsaved)',
        openTitle: 'Open rvdl file',
        saveTitle: 'Save rvdl file',
        qkFilter: 'Ravendrel Script',
        allFilter: 'All Files',
        daemonError: "Daemon connection failed: {message}\nIs 'runtime --daemon' running?",
        closeTitle: 'Unsaved changes',
        closeSave: 'Save',
        closeDiscard: 'Discard',
        closeCancel: 'Cancel',
    },
    zh: {
        windowTitle: 'SE — rvdl 编辑器',
        dirtySuffix: ' (未保存)',
        openTitle: '打开 rvdl 文件',
        saveTitle: '保存 rvdl 文件',
        qkFilter: 'Ravendrel 脚本',
        allFilter: '所有文件',
        daemonError: "守护进程连接失败：{message}\n请确认 'runtime --daemon' 已在运行",
        closeTitle: '有未保存的更改',
        closeSave: '保存',
        closeDiscard: '不保存',
        closeCancel: '取消',
    },
    ru: {
        windowTitle: 'SE — редактор rvdl',
        dirtySuffix: ' (не сохранено)',
        openTitle: 'Открыть файл rvdl',
        saveTitle: 'Сохранить файл rvdl',
        qkFilter: 'Скрипт Ravendrel',
        allFilter: 'Все файлы',
        daemonError: "Не удалось подключиться к демону: {message}\nУбедитесь, что 'runtime --daemon' запущен",
        closeTitle: 'Есть несохранённые изменения',
        closeSave: 'Сохранить',
        closeDiscard: 'Не сохранять',
        closeCancel: 'Отмена',
    },
    fr: {
        windowTitle: 'SE — éditeur rvdl',
        dirtySuffix: ' (non enregistré)',
        openTitle: 'Ouvrir un fichier rvdl',
        saveTitle: 'Enregistrer le fichier rvdl',
        qkFilter: 'Script Ravendrel',
        allFilter: 'Tous les fichiers',
        daemonError:
            "Échec de connexion au démon : {message}\nVérifiez que 'runtime --daemon' est en cours d'exécution",
        closeTitle: 'Modifications non enregistrées',
        closeSave: 'Enregistrer',
        closeDiscard: 'Ne pas enregistrer',
        closeCancel: 'Annuler',
    },
    de: {
        windowTitle: 'SE — rvdl-Editor',
        dirtySuffix: ' (ungespeichert)',
        openTitle: 'rvdl-Datei öffnen',
        saveTitle: 'rvdl-Datei speichern',
        qkFilter: 'Ravendrel-Skript',
        allFilter: 'Alle Dateien',
        daemonError:
            "Verbindung zum Daemon fehlgeschlagen: {message}\nStellen Sie sicher, dass 'runtime --daemon' läuft",
        closeTitle: 'Ungespeicherte Änderungen',
        closeSave: 'Speichern',
        closeDiscard: 'Verwerfen',
        closeCancel: 'Abbrechen',
    },
};

// ── 菜单栏文案 ──
const MENU_STRINGS: Record<
    Locale,
    {
        file: string;
        new: string;
        open: string;
        save: string;
        quit: string;
        edit: string;
        view: string;
        run: string;
        devtools: string;
        undo: string;
        redo: string;
        cut: string;
        copy: string;
        paste: string;
        selectAll: string;
        reload: string;
    }
> = {
    en: {
        file: 'File',
        new: 'New',
        open: 'Open',
        save: 'Save',
        quit: 'Quit',
        edit: 'Edit',
        view: 'View',
        run: 'Run',
        devtools: 'Toggle Developer Tools',
        undo: 'Undo',
        redo: 'Redo',
        cut: 'Cut',
        copy: 'Copy',
        paste: 'Paste',
        selectAll: 'Select All',
        reload: 'Reload',
    },
    zh: {
        file: '文件',
        new: '新建',
        open: '打开',
        save: '保存',
        quit: '退出',
        edit: '编辑',
        view: '视图',
        run: '运行',
        devtools: '开发者工具',
        undo: '撤销',
        redo: '重做',
        cut: '剪切',
        copy: '复制',
        paste: '粘贴',
        selectAll: '全选',
        reload: '重新加载',
    },
    ru: {
        file: 'Файл',
        new: 'Создать',
        open: 'Открыть',
        save: 'Сохранить',
        quit: 'Выход',
        edit: 'Правка',
        view: 'Вид',
        run: 'Запустить',
        devtools: 'Инструменты разработчика',
        undo: 'Отменить',
        redo: 'Повторить',
        cut: 'Вырезать',
        copy: 'Копировать',
        paste: 'Вставить',
        selectAll: 'Выделить всё',
        reload: 'Перезагрузить',
    },
    fr: {
        file: 'Fichier',
        new: 'Nouveau',
        open: 'Ouvrir',
        save: 'Enregistrer',
        quit: 'Quitter',
        edit: 'Édition',
        view: 'Affichage',
        run: 'Exécuter',
        devtools: 'Outils de développement',
        undo: 'Annuler',
        redo: 'Rétablir',
        cut: 'Couper',
        copy: 'Copier',
        paste: 'Coller',
        selectAll: 'Tout sélectionner',
        reload: 'Recharger',
    },
    de: {
        file: 'Datei',
        new: 'Neu',
        open: 'Öffnen',
        save: 'Speichern',
        quit: 'Beenden',
        edit: 'Bearbeiten',
        view: 'Ansicht',
        run: 'Ausführen',
        devtools: 'Entwicklertools',
        undo: 'Rückgängig',
        redo: 'Wiederholen',
        cut: 'Ausschneiden',
        copy: 'Kopieren',
        paste: 'Einfügen',
        selectAll: 'Alles auswählen',
        reload: 'Neu laden',
    },
};

function detectLocale(): Locale {
    const l = (app.getLocale() || 'en').toLowerCase();
    if (l.startsWith('zh')) return 'zh';
    if (l.startsWith('ru')) return 'ru';
    if (l.startsWith('fr')) return 'fr';
    if (l.startsWith('de')) return 'de';
    return 'en';
}

let currentLocale: Locale = 'en';
let isDirty = false;
let pendingCloseAfterSave = false;

function applyWindowTitle() {
    if (!win) return;
    const s = MAIN_STRINGS[currentLocale];
    win.setTitle(isDirty ? s.windowTitle + s.dirtySuffix : s.windowTitle);
}

function sendMenu(action: string) {
    if (win) win.webContents.send('SE:menu', action);
}

function buildMenu() {
    const m = MENU_STRINGS[currentLocale];
    Menu.setApplicationMenu(
        Menu.buildFromTemplate([
            {
                label: m.file,
                submenu: [
                    { label: m.new, click: () => sendMenu('new') },
                    { label: m.open, click: () => sendMenu('open') },
                    { label: m.save, click: () => sendMenu('save') },
                    { type: 'separator' },
                    { role: 'quit', label: m.quit },
                ],
            },
            {
                label: m.edit,
                submenu: [
                    { role: 'undo', label: m.undo },
                    { role: 'redo', label: m.redo },
                    { type: 'separator' },
                    { role: 'cut', label: m.cut },
                    { role: 'copy', label: m.copy },
                    { role: 'paste', label: m.paste },
                    { role: 'selectAll', label: m.selectAll },
                ],
            },
            {
                label: m.view,
                submenu: [
                    { label: m.run, click: () => sendMenu('run') },
                    { type: 'separator' },
                    { role: 'reload', label: m.reload },
                    { role: 'toggleDevTools', label: m.devtools },
                ],
            },
        ])
    );
}

function createWindow() {
    win = new BrowserWindow({
        width: 1100,
        height: 760,
        title: MAIN_STRINGS[currentLocale].windowTitle,
        icon: path.join(app.getAppPath(), 'icon.png'),
        webPreferences: {
            preload: path.join(__dirname, 'preload.js'),
            contextIsolation: true,
            nodeIntegration: false,
        },
    });
    win.loadFile(path.join(app.getAppPath(), 'renderer', 'index.html'));
    win.on('close', (e) => {
        if (!isDirty) return;
        const s = MAIN_STRINGS[currentLocale];
        const choice = dialog.showMessageBoxSync(win!, {
            type: 'warning',
            buttons: [s.closeSave, s.closeDiscard, s.closeCancel],
            defaultId: 2,
            cancelId: 2,
            message: s.closeTitle,
            detail: s.dirtySuffix,
        });
        if (choice === 2) {
            e.preventDefault();
            return;
        } // 取消
        if (choice === 0) {
            // 保存后关闭
            e.preventDefault();
            pendingCloseAfterSave = true;
            win!.webContents.send('SE:requestSave');
        }
    });
    win.on('closed', () => {
        win = null;
    });
}

ipcMain.handle('SE:run', async (_event, code: string) => {
    const { compileRvdl } = await import('./pipeline');
    const { runOnDaemon } = await import('./runner');
    const res = compileRvdl(code);
    if (!res.ok) return { ok: false, output: res.errors.join('\n') };
    try {
        const out = await runOnDaemon(res.llvmIR!);
        return { ok: true, output: out };
    } catch (e) {
        const msg = e instanceof Error ? e.message : String(e);
        return { ok: false, output: MAIN_STRINGS[currentLocale].daemonError.replace('{message}', msg) };
    }
});

ipcMain.handle('SE:open', async () => {
    if (!win) return null;
    const s = MAIN_STRINGS[currentLocale];
    const result = await dialog.showOpenDialog(win, {
        title: s.openTitle,
        filters: [
            { name: s.qkFilter, extensions: ['rvdl'] },
            { name: s.allFilter, extensions: ['*'] },
        ],
        properties: ['openFile'],
    });
    if (result.canceled || result.filePaths.length === 0) return null;
    const filePath = result.filePaths[0];
    try {
        const content = fs.readFileSync(filePath, 'utf-8');
        return { path: filePath, content };
    } catch (e) {
        return { error: e instanceof Error ? e.message : String(e) };
    }
});

ipcMain.handle('SE:save', async (_event, currentPath: string | null, content: string) => {
    if (!win) return null;
    let filePath = currentPath;
    if (!filePath) {
        const s = MAIN_STRINGS[currentLocale];
        const result = await dialog.showSaveDialog(win, {
            title: s.saveTitle,
            defaultPath: 'untitled.rvdl',
            filters: [{ name: s.qkFilter, extensions: ['rvdl'] }],
        });
        if (result.canceled || !result.filePath) return null;
        filePath = result.filePath;
    }
    try {
        fs.writeFileSync(filePath, content, 'utf-8');
        return filePath;
    } catch (e) {
        return { error: e instanceof Error ? e.message : String(e) };
    }
});

ipcMain.on('SE:setDirty', (_event, dirty: boolean) => {
    isDirty = dirty;
    applyWindowTitle();
});

ipcMain.on('SE:setLocale', (_event, locale: string) => {
    if (locale in MAIN_STRINGS) {
        currentLocale = locale as Locale;
        applyWindowTitle();
        buildMenu();
    }
});

ipcMain.on('SE:saveDone', () => {
    if (!pendingCloseAfterSave) return;
    pendingCloseAfterSave = false;
    if (win && !isDirty) win.close();
});

ipcMain.handle('SE:confirmDiscard', async () => {
    if (!win) return 'cancel';
    const s = MAIN_STRINGS[currentLocale];
    const choice = dialog.showMessageBoxSync(win, {
        type: 'warning',
        buttons: [s.closeSave, s.closeDiscard, s.closeCancel],
        defaultId: 2,
        cancelId: 2,
        message: s.closeTitle,
        detail: s.dirtySuffix,
    });
    return choice === 0 ? 'save' : choice === 1 ? 'discard' : 'cancel';
});

app.whenReady().then(() => {
    currentLocale = detectLocale();
    buildMenu();
    createWindow();
    app.on('activate', () => {
        if (BrowserWindow.getAllWindows().length === 0) createWindow();
    });
});

app.on('window-all-closed', () => {
    if (process.platform !== 'darwin') app.quit();
});
