// ============================================================================
// rvdl 结构化类型系统
//
// 用结构化 Type（判别联合）取代 semantic.ts 中的字符串类型，为类型检查、
// 泛型统一（cap<T> / lattice<T,B>）与函数签名表提供单一事实来源。
// ============================================================================

export type Type =
    | { kind: 'int'; width: 8 | 16 | 32 | 64; signed: boolean }
    | { kind: 'float'; width: 32 | 64 }
    | { kind: 'complex'; width: 32 | 64 }
    | { kind: 'bool' }
    | { kind: 'char' }
    | { kind: 'string' }
    | { kind: 'void' }
    | { kind: 'null' }
    | { kind: 'quantum'; cls: 'Qubit' | 'QObject' | 'QModel' | 'QReservoir' }
    | { kind: 'cap'; inner: Type }
    | { kind: 'lattice'; elem: Type; boundary: string }
    | { kind: 'array'; elem: Type; len: number }
    | { kind: 'func'; params: Type[]; ret: Type }
    | { kind: 'form'; name: string }
    | { kind: 'unknown' };

// ---- 构造器（单例常量） ----------------------------------------------------

export const T = {
    int8: { kind: 'int', width: 8, signed: true } as Type,
    int16: { kind: 'int', width: 16, signed: true } as Type,
    int32: { kind: 'int', width: 32, signed: true } as Type,
    int64: { kind: 'int', width: 64, signed: true } as Type,
    uint8: { kind: 'int', width: 8, signed: false } as Type,
    uint16: { kind: 'int', width: 16, signed: false } as Type,
    uint32: { kind: 'int', width: 32, signed: false } as Type,
    uint64: { kind: 'int', width: 64, signed: false } as Type,
    float: { kind: 'float', width: 32 } as Type,
    double: { kind: 'float', width: 64 } as Type,
    complex64: { kind: 'complex', width: 32 } as Type,
    complex128: { kind: 'complex', width: 64 } as Type,
    bool: { kind: 'bool' } as Type,
    char: { kind: 'char' } as Type,
    string: { kind: 'string' } as Type,
    void: { kind: 'void' } as Type,
    null: { kind: 'null' } as Type,
    unknown: { kind: 'unknown' } as Type,
    qubit: { kind: 'quantum', cls: 'Qubit' } as Type,
    qobject: { kind: 'quantum', cls: 'QObject' } as Type,
    qmodel: { kind: 'quantum', cls: 'QModel' } as Type,
    qreservoir: { kind: 'quantum', cls: 'QReservoir' } as Type,
};

export function cap(inner: Type): Type {
    return { kind: 'cap', inner };
}

export function lattice(elem: Type, boundary: string = 'open'): Type {
    return { kind: 'lattice', elem, boundary };
}

export function array(elem: Type, len: number): Type {
    return { kind: 'array', elem, len };
}

export function func(params: Type[], ret: Type): Type {
    return { kind: 'func', params, ret };
}

export function form(name: string): Type {
    return { kind: 'form', name };
}

// ---- 解析：ravendrel 源码类型字符串 → Type ------------------------------------

const INT_MAP: Record<string, Type> = {
    int8: T.int8, int16: T.int16, int: T.int32, int32: T.int32, int64: T.int64,
    uint8: T.uint8, uint16: T.uint16, uint32: T.uint32, uint64: T.uint64,
};

const QUANTUM_MAP: Record<string, Type> = {
    Qubit: T.qubit, QObject: T.qobject, QModel: T.qmodel, QReservoir: T.qreservoir,
};

/**
 * 把源码中的类型字符串解析为结构化 Type。
 * - `auto`/`let` → unknown（触发类型推导）
 * - `cap<T>` / `lattice<T, B>` → 递归解析泛型参数
 * - `(p1, p2)->ret` → 函数类型
 * - 其余（未识别的标识符）→ form（用户自定义类型）
 */
export function parseType(s: string): Type {
    if (s === 'auto' || s === 'let') return T.unknown;

    if (s === 'float') return T.float;
    if (s === 'double') return T.double;
    if (s === 'complex64') return T.complex64;
    if (s === 'complex128') return T.complex128;
    if (s === 'string') return T.string;
    if (s === 'char') return T.char;
    if (s === 'bool') return T.bool;
    if (s === 'void') return T.void;
    if (s === 'null') return T.null;

    if (INT_MAP[s]) return INT_MAP[s];
    if (QUANTUM_MAP[s]) return QUANTUM_MAP[s];

    if (s.startsWith('cap<') && s.endsWith('>')) {
        return cap(parseType(s.slice(4, -1).trim()));
    }

    if (s.startsWith('lattice<') && s.endsWith('>')) {
        const inner = s.slice('lattice<'.length, -1);
        const parts = inner.split(',');
        const elem = parseType(parts[0].trim());
        const boundary = parts.length > 1 ? parts.slice(1).join(',').trim() : 'open';
        return lattice(elem, boundary);
    }

    if (s.startsWith('arr<') && s.endsWith('>')) {
        const inner = s.slice('arr<'.length, -1);
        const parts = inner.split(',');
        const elem = parseType(parts[0].trim());
        const len = parts.length > 1 ? parseInt(parts[1].trim(), 10) : 0;
        return array(elem, Number.isFinite(len) ? len : 0);
    }

    if (s.startsWith('fn<') && s.endsWith('>')) {
        // fn<ret(p1, p2)> —— 函数指针类型（与 (p1, p2)->ret 等价）
        const inner = s.slice('fn<'.length, -1).trim();
        const openParen = inner.indexOf('(');
        const ret = parseType(inner.slice(0, openParen < 0 ? inner.length : openParen).trim());
        const params: Type[] = [];
        if (openParen >= 0) {
            const paramsStr = inner.slice(openParen + 1, inner.lastIndexOf(')')).trim();
            if (paramsStr.length > 0) {
                for (const p of paramsStr.split(',')) {
                    const pt = p.trim();
                    if (pt) params.push(parseType(pt));
                }
            }
        }
        return func(params, ret);
    }

    if (s.startsWith('(') && s.includes(')->')) {
        const arrowIdx = s.indexOf(')->');
        const paramsStr = s.slice(1, arrowIdx).trim();
        const retStr = s.slice(arrowIdx + 3).trim();
        const params: Type[] = [];
        if (paramsStr.length > 0) {
            for (const p of paramsStr.split(',')) {
                const pt = p.trim();
                if (pt) params.push(parseType(pt));
            }
        }
        return func(params, parseType(retStr));
    }

    return form(s);
}

// ---- 序列化：Type → 诊断字符串 --------------------------------------------

export function typeToString(t: Type): string {
    switch (t.kind) {
        case 'int': return t.signed ? `int${t.width}` : `uint${t.width}`;
        case 'float': return t.width === 32 ? 'float' : 'double';
        case 'complex': return t.width === 32 ? 'complex64' : 'complex128';
        case 'bool': return 'bool';
        case 'char': return 'char';
        case 'string': return 'string';
        case 'void': return 'void';
        case 'null': return 'null';
        case 'quantum': return t.cls;
        case 'cap': return `cap<${typeToString(t.inner)}>`;
        case 'lattice': return `lattice<${typeToString(t.elem)}, ${t.boundary}>`;
        case 'array': return `arr<${typeToString(t.elem)}, ${t.len}>`;
        case 'func': return `(${t.params.map(typeToString).join(', ')})->${typeToString(t.ret)}`;
        case 'form': return t.name;
        case 'unknown': return 'unknown';
    }
}

// ---- 类型相等 / 兼容性 ----------------------------------------------------

/** 结构相等；unknown 视为可匹配任意类型（通配） */
export function typeEquals(a: Type, b: Type): boolean {
    if (a.kind === 'unknown' || b.kind === 'unknown') return true;
    if (a.kind !== b.kind) return false;
    switch (a.kind) {
        case 'int': {
            const bb = b as Extract<Type, { kind: 'int' }>;
            return a.width === bb.width && a.signed === bb.signed;
        }
        case 'float': {
            const bb = b as Extract<Type, { kind: 'float' }>;
            return a.width === bb.width;
        }
        case 'complex': {
            const bb = b as Extract<Type, { kind: 'complex' }>;
            return a.width === bb.width;
        }
        case 'quantum': {
            const bb = b as Extract<Type, { kind: 'quantum' }>;
            return a.cls === bb.cls;
        }
        case 'cap': {
            const bb = b as Extract<Type, { kind: 'cap' }>;
            return typeEquals(a.inner, bb.inner);
        }
        case 'lattice': {
            const bb = b as Extract<Type, { kind: 'lattice' }>;
            return typeEquals(a.elem, bb.elem) && a.boundary === bb.boundary;
        }
        case 'array': {
            const bb = b as Extract<Type, { kind: 'array' }>;
            return a.len === bb.len && typeEquals(a.elem, bb.elem);
        }
        case 'func': {
            const bb = b as Extract<Type, { kind: 'func' }>;
            return a.params.length === bb.params.length
                && a.params.every((p, i) => typeEquals(p, bb.params[i]))
                && typeEquals(a.ret, bb.ret);
        }
        case 'form': {
            const bb = b as Extract<Type, { kind: 'form' }>;
            return a.name === bb.name;
        }
        case 'bool':
        case 'char':
        case 'string':
        case 'void':
        case 'null':
            return true;
        // 'unknown' 已在函数开头（a.kind==='unknown' || b.kind==='unknown'）提前返回
    }
}

export function isNumeric(t: Type): boolean {
    return t.kind === 'int' || t.kind === 'float' || t.kind === 'complex';
}

/** 数值类型之间允许隐式转换（如 int32 → double） */
export function isNumericCoercible(a: Type, b: Type): boolean {
    return isNumeric(a) && isNumeric(b);
}

/** 赋值兼容性：相等，或数值隐式转换，或 null→cap */
export function isAssignable(target: Type, source: Type): boolean {
    if (typeEquals(target, source)) return true;
    if (isNumericCoercible(target, source)) return true;
    if (source.kind === 'null' && target.kind === 'cap') return true;
    if (target.kind === 'cap' && isNumeric(source)) return true; // int → cap（MMIO 等）
    // func → cap<uint8>：函数指针可降为原始字节指针（中断表 / IDT / 回调表存储）
    if (source.kind === 'func' && target.kind === 'cap' &&
        target.inner.kind === 'int' && target.inner.width === 8 && !target.inner.signed) return true;
    // int → func：整型地址构造为函数指针（inttoptr），供 qload 运行时加载模块后
    // 动态调用入口（addr(&fn) 返回 int，反向往 fn<> 恢复为可调用函数指针）。
    if (target.kind === 'func' && isNumeric(source)) return true;
    // cap<T1> → cap<T2>：指针元素类型重解释（如 cap<int32> → cap<int8>，
    // rvdl_gc_alloc 返回 cap<int32>，赋给 cap<int8> 变量时需 bitcast）。
    if (target.kind === 'cap' && source.kind === 'cap') return true;
    return false;
}

/** 布尔/数值条件兼容（if/while/for 条件） */
export function isConditionType(t: Type): boolean {
    return t.kind === 'bool' || isNumeric(t) || t.kind === 'unknown';
}