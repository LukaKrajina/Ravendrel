// ============================================================================
// index.ts —— 迁移统一入口：源语言 → CircuitIR → .rvdl
//
// 各前端把源语言解析为 CircuitIR，再由 emitter 发射为 .rvdl 源。
// 支持显式 --from 指定，或按源内容自动探测。
// ============================================================================

import { CircuitIR } from './circuit-ir';
import { emitCircuit } from './emitter';
import { parseQasm2 } from './qasm2';
import { parseQasm3 } from './qasm3';
import { parseQuil } from './quil';
import { parseQsharp } from './qsharp';
import { parseSilq } from './silq';

export type SourceLang = 'openqasm2' | 'openqasm3' | 'qsharp' | 'quil' | 'silq';

/** 把某源语言的源码迁移为 .rvdl 源码字符串。 */
export function migrate(src: string, lang: SourceLang): string {
    return emitCircuit(parseSource(src, lang));
}

function parseSource(src: string, lang: SourceLang): CircuitIR {
    switch (lang) {
        case 'openqasm2':
            return parseQasm2(src);
        case 'openqasm3':
            return parseQasm3(src);
        case 'qsharp':
            return parseQsharp(src);
        case 'quil':
            return parseQuil(src);
        case 'silq':
            return parseSilq(src);
        default:
            throw new Error(`[rvdl migrate] Unsupported source language '${lang}'`);
    }
}

/** 按源内容自动探测语言；无法确定时返回 null。 */
export function detectLang(src: string): SourceLang | null {
    if (/OPENQASM\s+3\.0/.test(src)) return 'openqasm3';
    if (/OPENQASM\s+2\.0/.test(src)) return 'openqasm2';
    if (/\bnamespace\b/.test(src) && /\boperation\b/.test(src)) return 'qsharp';
    if (/\bDECLARE\b/.test(src) || /\bDEFGATE\b/.test(src)) return 'quil';
    return null;
}