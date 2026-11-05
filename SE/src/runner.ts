import * as net from 'net';
import { Cmd, PROTOCOL_VERSION, DAEMON_PORT, encodeFrame, decodeFrames } from '../../server/src/protocol';

// daemon 主机：默认回环，可经 RVDL_DAEMON_HOST 覆盖（远程 daemon 部署）。
// 端口的单一来源是 server/src/protocol.ts 的 DAEMON_PORT。
const DAEMON_HOST = process.env.RVDL_DAEMON_HOST || '127.0.0.1';
// 连接/响应超时：收到数据即重置（长编译不误杀），彻底无响应则拒绝。
const DAEMON_TIMEOUT_MS = 5000;

/**
 * 在 Ravendrel Daemon 上编译并执行 LLVM IR（rvdl_main）。
 * 使用长度前缀二进制帧协议（与 server/src/cli.ts 一致）：
 *   HELLO → COMPILE → EXECUTE → EXIT；响应为无命令字节的文本帧。
 * 仅在 daemon 返回至少一段非空响应时 resolve；空响应视为失败。
 */
export function runOnDaemon(llvmIR: string): Promise<string> {
    return new Promise((resolve, reject) => {
        const client = net.createConnection({ host: DAEMON_HOST, port: DAEMON_PORT });

        let settled = false;
        const finish = (fn: () => void) => {
            if (!settled) {
                settled = true;
                fn();
            }
        };

        let timer: NodeJS.Timeout | null = null;
        const resetTimeout = () => {
            if (timer) clearTimeout(timer);
            timer = setTimeout(() => {
                client.destroy();
                finish(() => reject(new Error('daemon timeout (no response in ' + DAEMON_TIMEOUT_MS + 'ms)')));
            }, DAEMON_TIMEOUT_MS);
        };
        resetTimeout();

        client.on('connect', () => {
            client.write(encodeFrame(Cmd.HELLO, PROTOCOL_VERSION));
            client.write(encodeFrame(Cmd.COMPILE, llvmIR));
            client.write(encodeFrame(Cmd.EXECUTE, 'int32 rvdl_main'));
            client.write(encodeFrame(Cmd.EXIT));
        });

        let recvBuffer: Buffer = Buffer.alloc(0);
        const chunks: string[] = [];
        client.on('data', (d: Buffer) => {
            resetTimeout();
            recvBuffer = Buffer.concat([recvBuffer, d]);
            const { frames, rest } = decodeFrames(recvBuffer);
            recvBuffer = rest;
            for (const f of frames) chunks.push(f);
        });
        client.on('end', () => {
            if (timer) clearTimeout(timer);
            const out = chunks.join('');
            finish(() =>
                out.trim().length > 0
                    ? resolve(out)
                    : reject(new Error('daemon returned an empty response')),
            );
        });
        client.on('error', (err) => {
            if (timer) clearTimeout(timer);
            finish(() => reject(err));
        });
    });
}
