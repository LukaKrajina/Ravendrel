import { db } from './db';
import { initI18n, setLocale, t, type Locale } from './i18n';

const API_BASE = (import.meta.env.VITE_RVDL_API_BASE as string | undefined) ?? 'http://localhost:9080';
const RVDL_API_URL = `${API_BASE}/v1/chat/completions`;
const MODELS_API_URL = `${API_BASE}/v1/models`;
let currentChatId: string | null = null;
let activeController: AbortController | null = null;

const elements = {
    input: document.getElementById('prompt-input') as HTMLTextAreaElement,
    sendBtn: document.getElementById('send-btn') as HTMLButtonElement,
    messagesContainer: document.getElementById('messages-container') as HTMLDivElement,
    welcomeScreen: document.getElementById('welcome-screen') as HTMLDivElement,
    historyList: document.getElementById('chat-history-list') as HTMLDivElement,
    newChatBtn: document.getElementById('new-chat-btn') as HTMLButtonElement,
    themeToggle: document.getElementById('theme-toggle') as HTMLButtonElement,
    themeIconLight: document.getElementById('theme-icon-light') as unknown as SVGElement,
    themeIconDark: document.getElementById('theme-icon-dark') as unknown as SVGElement,
    attachBtn: document.getElementById('attach-btn') as HTMLButtonElement,
    fileInput: document.getElementById('file-input') as HTMLInputElement,
    menuBtn: document.getElementById('menu-btn') as HTMLButtonElement,
    sidebar: document.getElementById('sidebar') as HTMLElement,
    collapseBtn: document.getElementById('collapse-btn') as HTMLButtonElement,
    modelSelect: document.getElementById('model-select') as HTMLSelectElement,
    overlay: document.getElementById('sidebar-overlay') as HTMLDivElement,
    languageSelect: document.getElementById('language-select') as HTMLSelectElement,
};

// ---------- 主题 ----------
function applyTheme(isDark: boolean) {
    if (isDark) {
        document.documentElement.classList.add('dark');
        elements.themeIconLight.classList.add('hidden');
        elements.themeIconDark.classList.remove('hidden');
    } else {
        document.documentElement.classList.remove('dark');
        elements.themeIconDark.classList.add('hidden');
        elements.themeIconLight.classList.remove('hidden');
    }
}

elements.themeToggle.addEventListener('click', () => {
    const isDark = !document.documentElement.classList.contains('dark');
    applyTheme(isDark);
    localStorage.setItem('ravendrel-theme', isDark ? 'dark' : 'light');
});

// ---------- 移动端抽屉 ----------
function openSidebar() {
    elements.sidebar.classList.remove('-translate-x-full');
    elements.sidebar.classList.add('translate-x-0');
    elements.overlay.classList.remove('opacity-0', 'pointer-events-none');
    elements.overlay.classList.add('opacity-100', 'pointer-events-auto');
}

function closeSidebar() {
    elements.sidebar.classList.add('-translate-x-full');
    elements.sidebar.classList.remove('translate-x-0');
    elements.overlay.classList.add('opacity-0', 'pointer-events-none');
    elements.overlay.classList.remove('opacity-100', 'pointer-events-auto');
}

// ---------- 欢迎页显隐 ----------
function syncWelcomeVisibility() {
    const hasMessages = elements.messagesContainer.children.length > 0;
    elements.welcomeScreen.classList.toggle('hidden', hasMessages);
    elements.messagesContainer.classList.toggle('hidden', !hasMessages);
}

// ---------- 消息 UI ----------
function appendMessageToUI(role: 'user' | 'assistant', content: string): HTMLElement {
    const wrapper = document.createElement('div');
    wrapper.className = `flex w-full ${role === 'user' ? 'justify-end' : 'justify-start'}`;

    const bubble = document.createElement('div');
    const baseClasses =
        'max-w-[85%] md:max-w-[75%] px-6 py-4 shadow-sm backdrop-blur-2xl border leading-relaxed transition-all';

    if (role === 'user') {
        bubble.className = `${baseClasses} rounded-3xl rounded-br-sm bg-blue-500/80 dark:bg-blue-600/80 text-white border-blue-400/30`;
    } else {
        bubble.className = `${baseClasses} rounded-3xl rounded-bl-sm bg-white/80 dark:bg-black/40 text-slate-800 dark:text-slate-100 border-white/60 dark:border-white/10 font-mono text-sm shadow-xl`;
    }

    bubble.innerText = content;
    wrapper.appendChild(bubble);
    elements.messagesContainer.appendChild(wrapper);
    elements.messagesContainer.scrollTop = elements.messagesContainer.scrollHeight;
    syncWelcomeVisibility();

    return bubble;
}

// ---------- 历史记录 ----------
async function loadChatHistory() {
    elements.historyList.innerHTML = '';
    const chats = await db.chats.orderBy('updatedAt').reverse().toArray();

    if (chats.length === 0) {
        const empty = document.createElement('p');
        empty.className = 'px-4 py-6 text-center text-xs text-slate-400 dark:text-slate-500';
        empty.innerText = t('history.empty');
        elements.historyList.appendChild(empty);
        return;
    }

    chats.forEach((chat) => {
        const wrapper = document.createElement('div');
        wrapper.className = 'group relative w-full';

        const btn = document.createElement('button');
        btn.className =
            'w-full text-left pl-4 pr-16 py-3 text-sm text-slate-700 dark:text-slate-300 hover:bg-white/60 dark:hover:bg-white/10 rounded-xl truncate transition-all border border-transparent hover:border-white/50 dark:hover:border-white/10 backdrop-blur-md shadow-sm';
        btn.innerText = chat.title;
        btn.onclick = () => loadSpecificChat(chat.id);

        const actions = document.createElement('div');
        actions.className =
            'absolute right-1 top-1/2 -translate-y-1/2 flex items-center gap-0.5 opacity-0 group-hover:opacity-100 transition-opacity';

        const renameBtn = document.createElement('button');
        renameBtn.className =
            'p-1.5 rounded-lg hover:bg-white dark:hover:bg-white/20 text-slate-400 hover:text-slate-600 dark:hover:text-slate-200 transition-all';
        renameBtn.title = t('history.rename');
        renameBtn.innerHTML =
            '<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17 3a2.828 2.828 0 1 1 4 4L7.5 20.5 2 22l1.5-5.5L17 3z"/></svg>';
        renameBtn.onclick = async (e) => {
            e.stopPropagation();
            const name = prompt(t('history.renamePrompt'), chat.title);
            if (name && name.trim()) {
                await db.chats.update(chat.id, { title: name.trim() });
                loadChatHistory();
            }
        };

        const delBtn = document.createElement('button');
        delBtn.className = 'p-1.5 rounded-lg hover:bg-red-500/20 text-slate-400 hover:text-red-500 transition-all';
        delBtn.title = t('history.delete');
        delBtn.innerHTML =
            '<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>';
        delBtn.onclick = async (e) => {
            e.stopPropagation();
            await db.chats.delete(chat.id);
            await db.messages.where('chatId').equals(chat.id).delete();
            if (currentChatId === chat.id) {
                currentChatId = null;
                elements.messagesContainer.innerHTML = '';
                syncWelcomeVisibility();
            }
            loadChatHistory();
        };

        actions.appendChild(renameBtn);
        actions.appendChild(delBtn);
        wrapper.appendChild(btn);
        wrapper.appendChild(actions);
        elements.historyList.appendChild(wrapper);
    });
}

async function loadSpecificChat(chatId: string) {
    currentChatId = chatId;
    elements.messagesContainer.innerHTML = '';
    closeSidebar();
    const messages = await db.messages.where('chatId').equals(chatId).sortBy('timestamp');
    messages.forEach((msg) => appendMessageToUI(msg.role, msg.content));
    syncWelcomeVisibility();
}

function startNewChat() {
    currentChatId = null;
    elements.messagesContainer.innerHTML = '';
    closeSidebar();
    syncWelcomeVisibility();
    elements.input.focus();
}

// ---------- 推理 ----------
async function buildMessages(prompt: string): Promise<Array<{ role: string; content: string }>> {
    const history: Array<{ role: string; content: string }> = [];
    if (currentChatId) {
        const msgs = await db.messages.where('chatId').equals(currentChatId).sortBy('timestamp');
        for (const m of msgs) history.push({ role: m.role, content: m.content });
    }
    history.push({ role: 'user', content: prompt });
    return history;
}

async function handleInference(prompt: string) {
    if (!currentChatId) {
        currentChatId = crypto.randomUUID();
        await db.chats.put({ id: currentChatId, title: prompt.substring(0, 25) + '...', updatedAt: Date.now() });
        loadChatHistory();
    }

    const messages = await buildMessages(prompt);

    appendMessageToUI('user', prompt);
    await db.messages.add({ chatId: currentChatId!, role: 'user', content: prompt, timestamp: Date.now() });

    const assistantBubble = appendMessageToUI('assistant', '');
    let fullResponse = '';

    elements.input.disabled = true;
    elements.sendBtn.disabled = true;

    activeController?.abort();
    activeController = new AbortController();

    try {
        const response = await fetch(RVDL_API_URL, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                model: elements.modelSelect.value,
                messages,
                stream: true,
            }),
            signal: activeController.signal,
        });

        if (!response.ok) throw new Error(`HTTP ${response.status} ${response.statusText}`);
        if (!response.body) throw new Error('No response body');

        const reader = response.body.getReader();
        const decoder = new TextDecoder('utf-8');
        let buffer = '';

        while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            buffer += decoder.decode(value, { stream: true });

            let idx;
            while ((idx = buffer.indexOf('\n')) !== -1) {
                const line = buffer.slice(0, idx).trim();
                buffer = buffer.slice(idx + 1);
                if (!line || !line.startsWith('data:')) continue;
                const payload = line.slice(5).trim();
                if (payload === '[DONE]') {
                    buffer = '';
                    break;
                }
                try {
                    const data = JSON.parse(payload);
                    const delta = data?.choices?.[0]?.delta?.content;
                    if (delta) {
                        fullResponse += delta;
                        assistantBubble.innerText = fullResponse;
                        elements.messagesContainer.scrollTop = elements.messagesContainer.scrollHeight;
                    }
                } catch {
                    // 忽略被分片截断/无法解析的行，等待后续 chunk 补齐
                }
            }
        }

        if (!fullResponse) fullResponse = '(empty response)';
        await db.messages.add({
            chatId: currentChatId!,
            role: 'assistant',
            content: fullResponse,
            timestamp: Date.now(),
        });
        await db.chats.update(currentChatId!, { updatedAt: Date.now() });
    } catch (err) {
        if (err instanceof Error && err.name === 'AbortError') {
            if (fullResponse) {
                await db.messages.add({
                    chatId: currentChatId!,
                    role: 'assistant',
                    content: fullResponse + '\n[stopped]',
                    timestamp: Date.now(),
                });
                await db.chats.update(currentChatId!, { updatedAt: Date.now() });
            }
        } else {
            const msg = err instanceof Error ? err.message : String(err);
            assistantBubble.innerText = `${t('error.connect')} (${msg})`;
            assistantBubble.classList.add('text-red-400');
        }
    } finally {
        activeController = null;
        elements.input.disabled = false;
        elements.sendBtn.disabled = false;
        elements.input.focus();
    }
}

// ---------- 模型 ----------
async function loadModels() {
    try {
        const res = await fetch(MODELS_API_URL);
        if (!res.ok) return;
        const data = (await res.json()) as { data?: Array<{ id: string }> };
        const models: string[] = (data.data ?? []).map((m) => m.id);
        if (models.length === 0) return;
        elements.modelSelect.innerHTML = '';
        models.forEach((m) => {
            const opt = document.createElement('option');
            opt.value = m;
            opt.textContent = m;
            elements.modelSelect.appendChild(opt);
        });
        const saved = localStorage.getItem('ravendrel-model');
        if (saved && models.includes(saved)) elements.modelSelect.value = saved;
    } catch {
        // 拉取失败则保留 index.html 中的默认选项
    }
}

// ---------- 事件 ----------
elements.sendBtn.addEventListener('click', () => {
    if (activeController) {
        activeController.abort();
        return;
    }
    const prompt = elements.input.value.trim();
    if (prompt) {
        elements.input.value = '';
        handleInference(prompt);
    }
});

elements.input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        elements.sendBtn.click();
    }
});

elements.newChatBtn.addEventListener('click', startNewChat);
elements.menuBtn.addEventListener('click', openSidebar);
elements.overlay.addEventListener('click', closeSidebar);

document.querySelectorAll('.suggestion-card, .chip').forEach((el) => {
    el.addEventListener('click', () => {
        const prompt = (el as HTMLElement).dataset.prompt || (el as HTMLElement).innerText.trim();
        elements.input.value = prompt;
        elements.input.focus();
    });
});

elements.attachBtn.addEventListener('click', () => elements.fileInput.click());
elements.fileInput.addEventListener('change', () => {
    const file = elements.fileInput.files?.[0];
    if (file) {
        elements.input.value += (elements.input.value ? '\n' : '') + `${t('attach.prefix')}${file.name}`;
        elements.input.focus();
    }
    elements.fileInput.value = '';
});

elements.collapseBtn.addEventListener('click', () => {
    const collapsed = elements.sidebar.classList.toggle('collapsed');
    localStorage.setItem('ravendrel-sidebar-collapsed', collapsed ? '1' : '0');
});

elements.modelSelect.addEventListener('change', () => {
    localStorage.setItem('ravendrel-model', elements.modelSelect.value);
});

elements.languageSelect.addEventListener('change', () => {
    setLocale(elements.languageSelect.value as Locale);
    // 历史列表为动态渲染，语言切换后需重刷
    loadChatHistory();
});

window.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
        e.preventDefault();
        startNewChat();
    }
    if (e.key === 'Escape') {
        closeSidebar();
    }
    if (
        e.key === '/' &&
        document.activeElement !== elements.input &&
        !(e.target instanceof HTMLTextAreaElement) &&
        !(e.target instanceof HTMLInputElement)
    ) {
        e.preventDefault();
        elements.input.focus();
    }
});

// ---------- 初始化 ----------
window.addEventListener('DOMContentLoaded', () => {
    const locale = initI18n();
    elements.languageSelect.value = locale;

    const savedTheme = localStorage.getItem('ravendrel-theme') || 'dark';
    applyTheme(savedTheme === 'dark');

    const savedModel = localStorage.getItem('ravendrel-model');
    if (savedModel) elements.modelSelect.value = savedModel;

    if (localStorage.getItem('ravendrel-sidebar-collapsed') === '1') {
        elements.sidebar.classList.add('collapsed');
    }

    loadChatHistory();
    loadModels();
    startNewChat();
});
