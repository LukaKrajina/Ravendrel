export enum Cmd {
    HELLO = 0x00,
    COMPILE = 0x01,
    EXECUTE = 0x02,
    VERIFY = 0x03,
    AOT_COMPILE = 0x04,
    LOAD_MMI = 0x05,
    MMI_INVOKE = 0x06,
    MMI_UNLOAD = 0x07,
    PING = 0x08,
    GET_SNAPSHOT = 0x09,
    LOAD_NATIVE = 0x0A,
    BIND_MMI = 0x0B,
    EXECUTE_TOPOLOGY = 0x0C,
    COMPILE_MIR = 0x0D,
    EXPORT_MODEL = 0x0E,
    IMPORT_MODEL = 0x0F,
    EXIT = 0xff
}

export const PROTOCOL_VERSION = 'RVDL_PROTO_V1';










export const DAEMON_PORT = 50052;

export function encodeFrame(cmd: Cmd, payload: string = ''): Buffer {
    const body = Buffer.concat([Buffer.from([cmd]), Buffer.from(payload, 'utf-8')]);
    const header = Buffer.alloc(4);
    header.writeUInt32BE(body.length, 0);
    return Buffer.concat([header, body]);
}

export function encodeResponseFrame(payload: string): Buffer {
    const header = Buffer.alloc(4);
    header.writeUInt32BE(Buffer.byteLength(payload, 'utf-8'), 0);
    return Buffer.concat([header, Buffer.from(payload, 'utf-8')]);
}

export function decodeFrames(buf: Buffer): { frames: string[]; rest: Buffer } {
    const frames: string[] = [];
    let offset = 0;
    while (buf.length - offset >= 4) {
        const len = buf.readUInt32BE(offset);
        if (buf.length - offset < 4 + len) break;
        frames.push(buf.subarray(offset + 4, offset + 4 + len).toString('utf-8'));
        offset += 4 + len;
    }
    return { frames, rest: buf.subarray(offset) };
}