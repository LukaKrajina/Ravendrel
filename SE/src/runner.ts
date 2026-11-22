import * as net from 'net';
import { Cmd, PROTOCOL_VERSION, DAEMON_PORT, encodeFrame, decodeFrames } from '../../server/src/protocol';

const DAEMON_HOST = process.env.RVDL_DAEMON_HOST || '127.0.0.1';
const DAEMON_TIMEOUT_MS = 5000;






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