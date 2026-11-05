// ============================================================================
// MIR 序列化：把 mir.ts 的 MirProgram（CFG 中间表示）序列化为 JSON，
// 供下沉到 LLVM C++ API 时通过协议传输（CMD_COMPILE_MIR）。
//
// 序列化格式（版本化，便于 C++ 端 MirModuleBuilder 做兼容）：
//   { version: 1, bodies: [ MirBody, ... ] }
//
// MirProgram 的所有字段均为纯数据（无函数 / Map），可直接 JSON 序列化。
// ============================================================================

import { MirProgram } from './mir';

export const MIR_FORMAT_VERSION = 1;

export interface SerializedMir {
    version: number;
    bodies: MirProgram['bodies'];
    forms?: MirProgram['forms'];
}

export function serializeMir(program: MirProgram): string {
    const wrapper: SerializedMir = {
        version: MIR_FORMAT_VERSION,
        bodies: program.bodies,
        ...(program.forms ? { forms: program.forms } : {}),
    };
    return JSON.stringify(wrapper);
}

export function deserializeMir(json: string): MirProgram {
    let parsed: unknown;
    try {
        parsed = JSON.parse(json);
    } catch {
        throw new Error('MIR: invalid JSON payload');
    }

    if (typeof parsed !== 'object' || parsed === null) {
        throw new Error('MIR: payload is not an object');
    }

    const obj = parsed as { version?: unknown; bodies?: unknown };

    if (obj.version !== MIR_FORMAT_VERSION) {
        throw new Error(`MIR: unsupported format version '${String(obj.version)}' (expected ${MIR_FORMAT_VERSION})`);
    }

    if (!Array.isArray(obj.bodies)) {
        throw new Error('MIR: missing or invalid bodies array');
    }

    // 逐 body 做最小结构校验，避免 C++ 端拿到畸形结构
    for (let i = 0; i < obj.bodies.length; i++) {
        const b = obj.bodies[i] as { owner?: unknown; blocks?: unknown; locals?: unknown };
        if (typeof b !== 'object' || b === null) {
            throw new Error(`MIR: bodies[${i}] is not an object`);
        }
        if (typeof b.owner !== 'string') {
            throw new Error(`MIR: bodies[${i}] missing 'owner' string`);
        }
        if (!Array.isArray(b.blocks)) {
            throw new Error(`MIR: bodies[${i}] ('${b.owner}') missing 'blocks' array`);
        }
        if (!Array.isArray(b.locals)) {
            throw new Error(`MIR: bodies[${i}] ('${b.owner}') missing 'locals' array`);
        }
    }

    return { bodies: obj.bodies as MirProgram['bodies'] };
}
