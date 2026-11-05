import { contextBridge, ipcRenderer } from 'electron';

contextBridge.exposeInMainWorld('SE', {
    runScript: (code: string) => ipcRenderer.invoke('SE:run', code),
    openFileDialog: () => ipcRenderer.invoke('SE:open'),
    saveFileDialog: (path: string | null, content: string) => ipcRenderer.invoke('SE:save', path, content),
    setDirty: (dirty: boolean) => ipcRenderer.send('SE:setDirty', dirty),
    setLocale: (locale: string) => ipcRenderer.send('SE:setLocale', locale),
    onMenu: (callback: (action: string) => void) => {
        ipcRenderer.on('SE:menu', (_event, action) => callback(action));
    },
    onRequestSave: (callback: () => void) => {
        ipcRenderer.on('SE:requestSave', () => callback());
    },
    notifySaveDone: () => ipcRenderer.send('SE:saveDone'),
    confirmDiscard: () => ipcRenderer.invoke('SE:confirmDiscard') as Promise<'save' | 'discard' | 'cancel'>,
});
