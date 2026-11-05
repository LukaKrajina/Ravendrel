// ============================================================================
// 帧格式: [4 字节大端长度][命令字节][payload]
// ============================================================================

export enum Cmd {
    HELLO = 0x00,        // 握手: payload = "RVDL_PROTO_V1"
    COMPILE = 0x01,      // payload = LLVM IR
    EXECUTE = 0x02,      // payload = "return_type func_name"
    VERIFY = 0x03,       // payload = VC protocol
    AOT_COMPILE = 0x04,  // payload = "arch mode name [clangxx=... sysroot=... rtdir=... extra_libs=...]\nIR"
    LOAD_MMI = 0x05,     // payload = path
    MMI_INVOKE = 0x06,   // payload = "id func_name args_json"
    MMI_UNLOAD = 0x07,   // payload = "id"
    PING = 0x08,
    GET_SNAPSHOT = 0x09,
    LOAD_NATIVE = 0x0A,   // payload = path（加载原生动态库，供 JIT 解析符号）
    BIND_MMI = 0x0B,      // payload = "alias path"（加载 .rvdli 并绑定到主 JIT）
    EXECUTE_TOPOLOGY = 0x0C, // payload = "int32 rvdl_topology_entry"（多维标签函数拓扑调度入口）
    COMPILE_MIR = 0x0D,      // payload = MIR JSON（下沉 LLVM C++ API：由 daemon 端 MirModuleBuilder 构建）
    EXPORT_MODEL = 0x0E,     // payload = "<verb> <in.rvdlm> <out> [tensor] [f32|f64|i32|i64|u8]"（.rvdlm → safetensors/gguf）
    IMPORT_MODEL = 0x0F,     // payload = "<verb> <in> <out.rvdlm> [tensor]"（safetensors/gguf → .rvdlm，类型按容器 dtype 推导）
    EXIT = 0xff
}

export const PROTOCOL_VERSION = 'RVDL_PROTO_V1';

/**
 * daemon 监听端口 —— **单一来源**。
 *
 * 曾在这个数字上摔过：它曾在 7 个文件里各写一遍（server.ts / cli.ts /
 * apiRouter.ts / SE runner / client sidebar / 两个 C++ 宿主），改一处漏一处
 * 的后果是「有的组件连得上、有的连不上」，而两半各自都看不出错。现在 TS 侧
 * 一律从这里 import；C++ 宿主（main_gui.cpp / rvdl_editor.hpp 的宏）与
 * VS Code 客户端（构建布局限制其无法 import server 源码）保留本地常量，
 * 但注释指向此处 —— 改端口时只改这一行 + 那两处宏。
 */
export const DAEMON_PORT = 50052;

// 编码一帧:[4 字节大端长度][命令字节][payload]
export function encodeFrame(cmd: Cmd, payload: string = ''): Buffer {
    const body = Buffer.concat([Buffer.from([cmd]), Buffer.from(payload, 'utf-8')]);
    const header = Buffer.alloc(4);
    header.writeUInt32BE(body.length, 0);
    return Buffer.concat([header, body]);
}

// 编码一个无命令字节的响应帧(daemon 响应格式,也用于测试)。
export function encodeResponseFrame(payload: string): Buffer {
    const header = Buffer.alloc(4);
    header.writeUInt32BE(Buffer.byteLength(payload, 'utf-8'), 0);
    return Buffer.concat([header, Buffer.from(payload, 'utf-8')]);
}

// 从累积 buffer 中解码所有完整帧,返回 { frames, rest }。
// frames 是各帧的完整 payload(长度头之后的内容)。
// 请求帧 payload = [命令字节][数据];响应帧 payload = 纯文本。
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