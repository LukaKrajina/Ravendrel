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