import { CircuitIR } from './circuit-ir';
import { emitCircuit } from './emitter';
import { parseQasm2 } from './qasm2';
import { parseQasm3 } from './qasm3';
import { parseQuil } from './quil';
import { parseQsharp } from './qsharp';
import { parseSilq } from './silq';

export type SourceLang = 'openqasm2' | 'openqasm3' | 'qsharp' | 'quil' | 'silq';

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

export function detectLang(src: string): SourceLang | null {
    if (/OPENQASM\s+3\.0/.test(src)) return 'openqasm3';
    if (/OPENQASM\s+2\.0/.test(src)) return 'openqasm2';
    if (/\bnamespace\b/.test(src) && /\boperation\b/.test(src)) return 'qsharp';
    if (/\bDECLARE\b/.test(src) || /\bDEFGATE\b/.test(src)) return 'quil';
    return null;
}