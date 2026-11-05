import en from './i18n/en.json';
import zh from './i18n/zh.json';
import ru from './i18n/ru.json';
import fr from './i18n/fr.json';
import de from './i18n/de.json';

export type Locale = 'en' | 'zh' | 'ru' | 'fr' | 'de';

export const LOCALES: ReadonlyArray<{ code: Locale; label: string }> = [
    { code: 'en', label: 'English' },
    { code: 'zh', label: '中文' },
    { code: 'ru', label: 'Русский' },
    { code: 'fr', label: 'Français' },
    { code: 'de', label: 'Deutsch' },
];

const dictionaries: Record<Locale, Record<string, string>> = { en, zh, ru, fr, de };

let current: Locale = 'en';

function detect(): Locale {
    const saved = localStorage.getItem('ravendrel-locale');
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
    localStorage.setItem('ravendrel-locale', locale);
    document.documentElement.lang = locale === 'zh' ? 'zh-CN' : locale;
    applyTranslations();
}

export function t(key: string): string {
    return dictionaries[current][key] ?? key;
}

export function applyTranslations(): void {
    document.querySelectorAll<HTMLElement>('[data-i18n]').forEach((el) => {
        el.textContent = t(el.dataset.i18n ?? '');
    });
    document.querySelectorAll<HTMLElement>('[data-i18n-placeholder]').forEach((el) => {
        el.setAttribute('placeholder', t(el.dataset.i18nPlaceholder ?? ''));
    });
    document.querySelectorAll<HTMLElement>('[data-i18n-title]').forEach((el) => {
        el.setAttribute('title', t(el.dataset.i18nTitle ?? ''));
    });
    document.querySelectorAll<HTMLElement>('[data-i18n-prompt]').forEach((el) => {
        el.dataset.prompt = t(el.dataset.i18nPrompt ?? '');
    });
}

export function initI18n(): Locale {
    current = detect();
    document.documentElement.lang = current === 'zh' ? 'zh-CN' : current;
    applyTranslations();
    return current;
}
