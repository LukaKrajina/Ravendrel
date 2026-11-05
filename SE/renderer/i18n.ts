import en from './i18n/en.json';
import zh from './i18n/zh.json';
import ru from './i18n/ru.json';
import fr from './i18n/fr.json';
import de from './i18n/de.json';

export type Locale = 'en' | 'zh' | 'ru' | 'fr' | 'de';

const dictionaries: Record<Locale, Record<string, string>> = { en, zh, ru, fr, de };

let current: Locale = 'en';

function detect(): Locale {
    const saved = localStorage.getItem('se-locale');
    if (saved && saved in dictionaries) return saved as Locale;
    const lang = (navigator.language || 'en').toLowerCase();
    if (lang.startsWith('zh')) return 'zh';
    if (lang.startsWith('ru')) return 'ru';
    if (lang.startsWith('fr')) return 'fr';
    if (lang.startsWith('de')) return 'de';
    return 'en';
}

export function getLocale(): Locale {
    return current;
}

export function setLocale(locale: Locale): void {
    current = locale;
    localStorage.setItem('se-locale', locale);
    document.documentElement.lang = locale === 'zh' ? 'zh-CN' : locale;
    applyTranslations();
    // 通知主进程同步（窗口标题 / 对话框文案）
    window.SE.setLocale(locale);
}

export function t(key: string): string {
    return dictionaries[current][key] ?? key;
}

export function applyTranslations(): void {
    document.querySelectorAll<HTMLElement>('[data-i18n]').forEach((el) => {
        el.textContent = t(el.dataset.i18n ?? '');
    });
}

export function initI18n(): Locale {
    current = detect();
    document.documentElement.lang = current === 'zh' ? 'zh-CN' : current;
    applyTranslations();
    window.SE.setLocale(current);
    return current;
}