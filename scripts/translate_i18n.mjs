#!/usr/bin/env node
/**
 * 自动化 i18n 翻译脚本（增量维护用）
 *
 * 读取一个英文基准 JSON（如 chatWebUI/src/i18n/en.json），
 * 批量翻译成中文 / 俄语 / 法语 / 德语，并写出对应语言的 JSON 文件。
 *
 * 支持两种机器翻译后端：
 *   1. LibreTranslate（自托管 HTTP 服务，默认）
 *   2. Argos Translate（开源离线，通过 Python 调用）
 *
 * 用法：
 *   node scripts/translate_i18n.mjs <基准en.json> <输出目录> [--backend libretranslate|argos]
 *
 * 环境变量：
 *   LIBRETRANSLATE_URL    LibreTranslate 服务地址（默认 http://localhost:5000）
 *   LIBRETRANSLATE_KEY    可选 API key
 *
 * 示例：
 *   node scripts/translate_i18n.mjs chatWebUI/src/i18n/en.json chatWebUI/src/i18n
 *   LIBRETRANSLATE_URL=http://localhost:5000 node scripts/translate_i18n.mjs en.json out --backend libretranslate
 *   node scripts/translate_i18n.mjs en.json out --backend argos
 */

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { execFileSync } from 'node:child_process';

const TARGETS = [
    { code: 'zh', argos: 'zh', name: '中文' },
    { code: 'ru', argos: 'ru', name: 'Русский' },
    { code: 'fr', argos: 'fr', name: 'Français' },
    { code: 'de', argos: 'de', name: 'Deutsch' },
];

function usage() {
    console.error('用法: node translate_i18n.mjs <基准en.json> <输出目录> [--backend libretranslate|argos]');
    process.exit(1);
}

// ─── LibreTranslate 后端 ──────────────────────────────────────────
async function translateLibre(text, target, url, key) {
    const body = { q: text, source: 'en', target, format: 'text' };
    if (key) body.api_key = key;
    const res = await fetch(`${url.replace(/\/$/, '')}/translate`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
    });
    if (!res.ok) {
        throw new Error(`LibreTranslate ${res.status}: ${await res.text()}`);
    }
    const data = await res.json();
    return data.translatedText ?? text;
}

// ─── Argos Translate 后端（离线，Python）──────────────────────────
function translateArgos(text, argosCode) {
    const py = `
import sys, json
try:
    import argostranslate.package, argostranslate.translate
    from argostranslate.translate import translation
except Exception as e:
    sys.stderr.write("argostranslate not installed: pip install argostranslate\\n")
    sys.exit(2)
text = json.loads(sys.stdin.read())
out = []
for t in text:
    out.append(translation.translate(t, "en", "${argosCode}"))
print(json.dumps(out, ensure_ascii=False))
`;
    const encoded = JSON.stringify([text]);
    const stdout = execFileSync('python3', ['-c', py], { input: encoded, encoding: 'utf-8' });
    const arr = JSON.parse(stdout);
    return arr[0] ?? text;
}

async function main() {
    const args = process.argv.slice(2);
    if (args.length < 2) usage();

    const basePath = resolve(args[0]);
    const outDir = resolve(args[1]);
    const backendArg = args.find((a) => a.startsWith('--backend'));
    const backend = backendArg ? backendArg.split('=')[1] : (process.env.TRANSLATE_BACKEND || 'libretranslate');

    const base = JSON.parse(readFileSync(basePath, 'utf-8'));
    const entries = Object.entries(base);
    console.log(`基准文件: ${basePath}（${entries.length} 个条目）`);
    console.log(`翻译后端: ${backend}\n`);

    mkdirSync(outDir, { recursive: true });

    const libUrl = process.env.LIBRETRANSLATE_URL || 'http://localhost:5000';
    const libKey = process.env.LIBRETRANSLATE_KEY || '';

    for (const { code, argos, name } of TARGETS) {
        const out = {};
        console.log(`→ 翻译 ${name} (${code}) ...`);
        let i = 0;
        for (const [key, value] of entries) {
            try {
                let translated = value;
                if (backend === 'argos') {
                    translated = translateArgos(value, argos);
                } else {
                    translated = await translateLibre(value, code, libUrl, libKey);
                }
                out[key] = translated;
            } catch (e) {
                console.error(`   [跳过] ${key}: ${e.message}`);
                out[key] = value; // 失败时回退原文，保证结构完整
            }
            if (++i % 10 === 0) process.stdout.write(`   ${i}/${entries.length}\r`);
        }
        const outFile = resolve(outDir, `${code}.json`);
        writeFileSync(outFile, JSON.stringify(out, null, 2) + '\n', 'utf-8');
        console.log(`   ✓ 已写出 ${outFile}`);
    }

    console.log('\n完成。请人工校对机器翻译结果（尤其术语与占位符 %1/%2/%3 等）。');
}

main().catch((e) => {
    console.error('错误:', e.message);
    process.exit(1);
});