import * as http from 'http';
import * as net from 'net';
import * as path from 'path';
import * as fs from 'fs';
import * as crypto from 'crypto';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { SemanticAnalyzer } from './semantic';
import { IRGenerator } from './ir';
import { withRetry, withTimeout } from './resilience';
import { Cmd, PROTOCOL_VERSION, DAEMON_PORT, encodeFrame, decodeFrames } from './protocol';
import { readMMIExports } from './mmi';

export interface ServeSecurityOptions {
    apiKey?: string;
    rateLimitPerMin?: number;
    auditLogPath?: string;
}





function tokenizeForHash(text: string): string[] {
    const tokens: string[] = [];
    const words = text.toLowerCase().split(/[^\p{L}\p{N}]+/u).filter(Boolean);
    for (const w of words) {
        tokens.push(w);
        for (let n = 2; n <= 3 && n <= w.length; n++) {
            for (let i = 0; i + n <= w.length; i++) tokens.push(w.slice(i, i + n));
        }
    }
    return tokens;
}

function fnv1a(s: string): number {
    let h = 0x811c9dc5;
    for (let i = 0; i < s.length; i++) {
        h ^= s.charCodeAt(i);
        h = Math.imul(h, 0x01000193);
    }
    return h >>> 0;
}

export class RavendrelApiRouter {
    private modelPath: string;
    private modelName: string;
    private visionModule: string;
    private readonly apiKey: string;
    private readonly rateLimitPerMin: number;
    private readonly auditLogPath: string;
    private rateState = new Map<string, { start: number; count: number }>();

    constructor(modelPath: string, visionModule = '', security: ServeSecurityOptions = {}) {
        this.modelPath = modelPath;
        this.modelName = path.basename(modelPath);
        this.visionModule = visionModule;
        this.apiKey = security.apiKey ?? '';
        this.rateLimitPerMin = security.rateLimitPerMin ?? 0;
        this.auditLogPath = security.auditLogPath ?? '';
    }

    private static keyMatches(candidate: string, expected: string): boolean {
        if (!candidate) return false;
        const a = crypto.createHash('sha256').update(candidate).digest();
        const b = crypto.createHash('sha256').update(expected).digest();
        return crypto.timingSafeEqual(a, b);
    }

    private authorized(req: http.IncomingMessage): boolean {
        if (!this.apiKey) return true;
        const auth = String(req.headers['authorization'] || '');
        const bearer = auth.toLowerCase().startsWith('bearer ') ? auth.slice(7).trim() : '';
        const headerKey = String(req.headers['x-api-key'] || '');
        return RavendrelApiRouter.keyMatches(bearer, this.apiKey)
            || RavendrelApiRouter.keyMatches(headerKey, this.apiKey);
    }

    private allowRate(ip: string): boolean {
        if (this.rateLimitPerMin <= 0) return true;
        const now = Date.now();
        if (this.rateState.size > 4096) {
            for (const [k, v] of this.rateState) {
                if (now - v.start >= 60_000) this.rateState.delete(k);
            }
        }
        const win = this.rateState.get(ip);
        if (!win || now - win.start >= 60_000) {
            this.rateState.set(ip, { start: now, count: 1 });
            return true;
        }
        if (win.count >= this.rateLimitPerMin) return false;
        win.count++;
        return true;
    }

    private attachAudit(req: http.IncomingMessage, res: http.ServerResponse, url: string): void {
        if (!this.auditLogPath) return;
        const entry: Record<string, unknown> = {
            ts: new Date().toISOString(),
            ip: req.socket.remoteAddress || '',
            path: url,
        };
        (res as any).__audit = entry;
        const started = Date.now();
        res.on('finish', () => {
            entry.status = res.statusCode;
            entry.ms = Date.now() - started;
            fs.appendFile(this.auditLogPath, JSON.stringify(entry) + '\n', () => undefined);
        });
    }

    public handleRequest(req: http.IncomingMessage, res: http.ServerResponse): void {
        res.setHeader('Access-Control-Allow-Origin', '*');
        res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
        res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization, x-api-key');

        if (req.method === 'OPTIONS') {
            res.writeHead(200);
            res.end();
            return;
        }

        const url = req.url || '';

        if (req.method === 'POST' && (url === '/v1/chat/completions' || url === '/v1/completions' || url === '/v1/predict' || url === '/v1/embeddings')) {
            this.attachAudit(req, res, url);
        }

        if (!this.authorized(req)) {
            res.writeHead(401, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({ error: { message: 'Unauthorized: missing or invalid API key.', type: 'auth_error' } }));
            return;
        }

        if (!this.allowRate(req.socket.remoteAddress || '')) {
            res.writeHead(429, { 'Content-Type': 'application/json', 'Retry-After': '60' });
            res.end(JSON.stringify({ error: { message: 'Rate limit exceeded.', type: 'rate_limit_error' } }));
            return;
        }

        if (req.method === 'GET' && (url === '/v1/models' || url === '/models')) {
            this.handleGetModels(res);
            return;
        }

        if (req.method === 'POST' && (url === '/v1/chat/completions' || url === '/v1/completions' || url === '/v1/predict')) {
            this.parseJsonBody(req, res, (body) => this.handleCompletions(body, res));
            return;
        }

        if (req.method === 'POST' && url === '/v1/embeddings') {
            this.parseJsonBody(req, res, (body) => this.handleEmbeddings(body, res));
            return;
        }

        res.writeHead(404, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: { message: `Route ${req.method} ${url} not found.`, type: 'invalid_request_error' } }));
    }

    private handleGetModels(res: http.ServerResponse): void {
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
            object: 'list',
            data: [{
                id: this.modelName,
                object: 'model',
                created: Math.floor(Date.now() / 1000),
                owned_by: 'ravendrel-qml'
            }]
        }));
    }

    private handleCompletions(body: any, res: http.ServerResponse): void {
        const mm = this.extractMultimodal(body);
        const audit = (res as any).__audit as Record<string, unknown> | undefined;
        if (audit) {
            audit.model = typeof body?.model === 'string' ? body.model : '';
            audit.hasImage = mm.images.length > 0;
            audit.promptChars = mm.text.length;
        }

        if (mm.images.length > 0) {
            this.handleVisionCompletions(body, res, mm.text, mm.images[0]);
            return;
        }

        let prompt = '';
        if (mm.text.length > 0) {
            prompt = mm.text;
        } else if (Array.isArray(body.messages)) {
            prompt = body.messages
                .map((m: any) => `${m.role}: ${m.content}`)
                .join('\n');
        } else if (typeof body.prompt === 'string') {
            prompt = body.prompt;
        } else {
            prompt = JSON.stringify(body);
        }

        const isStream = body.stream === true;
        const rvdlScript = `
            let model = qlm_load(${JSON.stringify(this.modelPath)});
            let input = rvdl_encode_string(${JSON.stringify(prompt)});
            qlm_forward(model, input);
            let result = rvdl_decode_string(input);
        `;

        this.executeRvdlScript(rvdlScript, (err, daemonOutput) => {
            if (err) {
                res.writeHead(500, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ error: { message: err, type: 'hardware_execution_error' } }));
                return;
            }

            const promptTokens = this.estimateTokens(prompt);
            const completionTokens = this.estimateTokens(daemonOutput);
            const usage = {
                prompt_tokens: promptTokens,
                completion_tokens: completionTokens,
                total_tokens: promptTokens + completionTokens
            };

            if (isStream) {
                res.writeHead(200, {
                    'Content-Type': 'text/event-stream',
                    'Cache-Control': 'no-cache',
                    'Connection': 'keep-alive'
                });

                const id = 'chatcmpl-' + Date.now();
                const created = Math.floor(Date.now() / 1000);
                const base = { id, object: 'chat.completion.chunk', created, model: this.modelName };
                const writeChunk = (choices: unknown[]) => {
                    res.write(`data: ${JSON.stringify({ ...base, choices })}\n\n`);
                };

                writeChunk([{ delta: { role: 'assistant' }, index: 0, finish_reason: null }]);
                if (daemonOutput.length > 0) {
                    writeChunk([{ delta: { content: daemonOutput }, index: 0, finish_reason: null }]);
                }
                writeChunk([{ delta: {}, index: 0, finish_reason: 'stop' }]);
                res.write(`data: ${JSON.stringify({ ...base, choices: [], usage })}\n\n`);
                res.write('data: [DONE]\n\n');
                res.end();
            } else {
                res.writeHead(200, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({
                    id: 'chatcmpl-' + Date.now(),
                    object: 'chat.completion',
                    created: Math.floor(Date.now() / 1000),
                    model: this.modelName,
                    choices: [{
                        message: { role: 'assistant', content: daemonOutput },
                        finish_reason: 'stop',
                        index: 0
                    }],
                    usage
                }));
            }
        });
    }

    private handleEmbeddings(body: any, res: http.ServerResponse): void {
        const raw = body?.input;
        const inputs: string[] = Array.isArray(raw)
            ? raw.map((x) => String(x))
            : (raw != null ? [String(raw)] : []);
        const audit = (res as any).__audit as Record<string, unknown> | undefined;
        if (audit) audit.inputs = inputs.length;
        if (inputs.length === 0) {
            res.writeHead(400, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({
                error: { message: 'Missing "input" field.', type: 'invalid_request_error' }
            }));
            return;
        }
        const dim = 384;
        const data = inputs.map((text, i) => ({
            object: 'embedding',
            index: i,
            embedding: this.featureHashEmbedding(text, dim),
        }));
        const promptTokens = inputs.reduce((n, t) => n + this.estimateTokens(t), 0);
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({
            object: 'list',
            model: this.modelName,
            data,
            usage: { prompt_tokens: promptTokens, total_tokens: promptTokens },
        }));
    }

    private featureHashEmbedding(text: string, dim: number): number[] {
        const vec = new Float64Array(dim);
        const tokens = tokenizeForHash(text);
        for (const tok of tokens) {
            const h = fnv1a(tok);
            const idx = h % dim;
            const sign = (h & 0x80000000) ? -1.0 : 1.0;
            vec[idx] += sign;
        }
        let norm = 0.0;
        for (let i = 0; i < dim; i++) norm += vec[i] * vec[i];
        norm = Math.sqrt(norm) || 1.0;
        return Array.from(vec, (v) => v / norm);
    }

    private estimateTokens(text: string): number {
        return text.length === 0 ? 0 : Math.ceil(text.length / 4);
    }




    private extractMultimodal(body: any): { text: string; images: string[] } {
        const images: string[] = [];
        let text = '';
        if (Array.isArray(body.messages)) {
            const parts: string[] = [];
            for (const m of body.messages) {
                const c = m?.content;
                if (typeof c === 'string') {
                    parts.push(`${m.role}: ${c}`);
                    continue;
                }
                if (Array.isArray(c)) {
                    const texts: string[] = [];
                    for (const p of c) {
                        if (typeof p === 'string') { texts.push(p); continue; }
                        if (p?.type === 'text' && typeof p.text === 'string') { texts.push(p.text); continue; }
                        if (p?.type === 'image_url') {
                            const url = typeof p.image_url === 'string' ? p.image_url : p.image_url?.url;
                            if (typeof url === 'string' && url.length > 0) images.push(url);
                        }
                    }
                    parts.push(`${m.role}: ${texts.join(' ')}`);
                }
            }
            text = parts.join('\n');
        }
        if (text.length === 0 && typeof body.prompt === 'string') text = body.prompt;
        return { text, images };
    }









    private handleVisionCompletions(body: any, res: http.ServerResponse, promptText: string, imageRef: string): void {
        if (!this.visionModule) {
            res.writeHead(400, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({
                error: {
                    message: '图像内容需要多模态模块：请用 rvdl serve <model.rvdlm> --module <vision.rvdli> 启动（如 jarvis_vision.rvdli）。',
                    type: 'invalid_request_error'
                }
            }));
            return;
        }

        const alias = 'jv';
        const context = 0.5;
        const rvdlScript = `
            import ${alias} from ${JSON.stringify(this.visionModule)};
            export int32 rvdl_vision_entry() {
                return ${alias}::jarvis_forward_mm_i32(${JSON.stringify(promptText)}, ${context}, ${JSON.stringify(imageRef)});
            }
        `;

        const isStream = body.stream === true;
        const robust = withRetry(3, { delayMs: 500, backoff: 'exponential' })(
            withTimeout(20_000)(() => this.executeVisionPipeline(
                rvdlScript,
                `${alias} ${this.visionModule}`,
                (evidence) => `
                    let model = qlm_load(${JSON.stringify(this.modelPath)});
                    let input = rvdl_encode_string(${JSON.stringify(`${promptText}\n[图像证据: ${evidence}]`)});
                    qlm_forward(model, input);
                    let result = rvdl_decode_string(input);
                `,
            ))
        );
        robust().then(
            (r) => {
                if (r.error) {
                    res.writeHead(500, { 'Content-Type': 'application/json' });
                    res.end(JSON.stringify({ error: { message: r.error, type: 'hardware_execution_error' } }));
                    return;
                }
                if (r.returned === null) {
                    this.respondVision(res, isStream, promptText, '（Vision 推理无返回值，请检查 daemon 与后端日志）');
                    return;
                }
                const content = r.text.length > 0 ? r.text : String(r.returned);
                this.respondVision(res, isStream, promptText, content);
            },
            (e) => {
                res.writeHead(500, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ error: { message: e instanceof Error ? e.message : String(e), type: 'hardware_execution_error' } }));
            },
        );
    }

    private respondVision(res: http.ServerResponse, isStream: boolean, promptText: string, content: string): void {
        const promptTokens = this.estimateTokens(promptText);
        const completionTokens = this.estimateTokens(content);
        const usage = {
            prompt_tokens: promptTokens,
            completion_tokens: completionTokens,
            total_tokens: promptTokens + completionTokens
        };
        if (isStream) {
            res.writeHead(200, {
                'Content-Type': 'text/event-stream',
                'Cache-Control': 'no-cache',
                'Connection': 'keep-alive'
            });
            const id = 'chatcmpl-' + Date.now();
            const created = Math.floor(Date.now() / 1000);
            const base = { id, object: 'chat.completion.chunk', created, model: this.modelName };
            const writeChunk = (choices: unknown[]) => {
                res.write(`data: ${JSON.stringify({ ...base, choices })}\n\n`);
            };
            writeChunk([{ delta: { role: 'assistant' }, index: 0, finish_reason: null }]);
            writeChunk([{ delta: { content }, index: 0, finish_reason: null }]);
            writeChunk([{ delta: {}, index: 0, finish_reason: 'stop' }]);
            res.write(`data: ${JSON.stringify({ ...base, choices: [], usage })}\n\n`);
            res.write('data: [DONE]\n\n');
            res.end();
        } else {
            res.writeHead(200, { 'Content-Type': 'application/json' });
            res.end(JSON.stringify({
                id: 'chatcmpl-' + Date.now(),
                object: 'chat.completion',
                created: Math.floor(Date.now() / 1000),
                model: this.modelName,
                choices: [{
                    message: { role: 'assistant', content },
                    finish_reason: 'stop',
                    index: 0
                }],
                usage
            }));
        }
    }

    private compileSource(sourceCode: string): string {
        const lexer = new Lexer(sourceCode);
        const parser = new Parser(lexer);
        const ast = parser.parse();

        const analyzer = new SemanticAnalyzer();
        analyzer.analyze(ast);

        if (analyzer.errors.length > 0) {
            throw new Error(analyzer.errors.map(e => e.message).join('; '));
        }

        const importSignatures = new Map<string, Map<string, { params: string[]; ret: string }>>();
        for (const node of ast.body) {
            if ((node as any).type === 'ImportDecl') {
                const imp = node as any;
                const sigs = readMMIExports(imp.path, path.dirname(this.modelPath));
                const funcs = new Map<string, { params: string[]; ret: string }>();
                for (const e of sigs) funcs.set(e.name, { params: e.params, ret: e.ret });
                importSignatures.set(imp.alias, funcs);
            }
        }

        const irGen = new IRGenerator();
        return irGen.generate(ast, importSignatures.size > 0 ? importSignatures : undefined);
    }








    private executeVisionPipeline(
        visionSource: string,
        bindPayload: string,
        qlmSourceFactory: (evidenceFixed: string) => string,
    ): Promise<{ returned: number | null; text: string; error: string | null }> {
        return new Promise((resolve, reject) => {
            try {
                const visionIR = this.compileSource(visionSource);

                const client = net.createConnection({ port: DAEMON_PORT }, () => {
                    client.write(encodeFrame(Cmd.HELLO, PROTOCOL_VERSION));
                    client.write(encodeFrame(Cmd.BIND_MMI, bindPayload));
                    client.write(encodeFrame(Cmd.COMPILE, visionIR));
                    client.write(encodeFrame(Cmd.EXECUTE, 'int32 rvdl_vision_entry'));
                });

                let stage1Done = false;
                let visionReturned: number | null = null;
                let recvBuffer: Buffer = Buffer.alloc(0);
                let acc = '';
                const fail = (msg: string): void => {
                    try { client.end(); } catch { }
                    reject(new Error(msg));
                };

                client.on('data', (data: Buffer) => {
                    recvBuffer = Buffer.concat([recvBuffer, data]);
                    const { frames, rest } = decodeFrames(recvBuffer);
                    recvBuffer = rest;
                    for (const f of frames) acc += f;

                    if (!stage1Done && acc.includes('Returned:')) {
                        const m = acc.match(/Returned:\s*(-?\d+)/);
                        if (!m) return;
                        stage1Done = true;
                        visionReturned = parseInt(m[1], 10);
                        acc = '';
                        try {
                            const qlmIR = this.compileSource(qlmSourceFactory((visionReturned / 1000).toString()));
                            client.write(encodeFrame(Cmd.COMPILE, qlmIR));
                            client.write(encodeFrame(Cmd.EXECUTE, 'int32 rvdl_main'));
                            client.write(encodeFrame(Cmd.EXIT));
                        } catch (e: any) {
                            fail(`Pipeline Compilation Error: ${e.message}`);
                        }
                    }
                });
                client.on('end', () => {
                    const text = acc
                        .split(/\r?\n/)
                        .filter((l) => !l.startsWith('RESPONSE:'))
                        .join('\n')
                        .trim();
                    resolve({ returned: visionReturned, text, error: null });
                });
                client.on('error', (err) => reject(new Error(`Daemon Connection Error: ${err.message}`)));
            } catch (e: any) {
                resolve({ returned: null, text: '', error: `Pipeline Compilation Error: ${e.message}` });
            }
        });
    }

    private executeRvdlScriptAsync(
        sourceCode: string,
        opts: { bindMmi?: string; captureReturned?: boolean } = {},
    ): Promise<{ output: string; returned: number | null }> {
        return new Promise<{ output: string; returned: number | null }>((resolve, reject) => {
            try {
                const ast = ((): any => {
                    const lexer = new Lexer(sourceCode);
                    const parser = new Parser(lexer);
                    return parser.parse();
                })();
                const llvmIR = this.compileSource(sourceCode);

                const client = net.createConnection({ port: DAEMON_PORT }, () => {
                    client.write(encodeFrame(Cmd.HELLO, PROTOCOL_VERSION));
                    if (opts.bindMmi) {
                        client.write(encodeFrame(Cmd.BIND_MMI, opts.bindMmi));
                    }
                    client.write(encodeFrame(Cmd.COMPILE, llvmIR));
                    const hasLayerFns = ast.body.some((n: any) => (n as any).type === 'FunctionDeclaration' && (n as any).layer);
                    if (hasLayerFns) {
                        client.write(encodeFrame(Cmd.EXECUTE_TOPOLOGY, 'int32 rvdl_topology_entry'));
                    } else {
                        client.write(encodeFrame(Cmd.EXECUTE, 'int32 rvdl_main'));
                    }
                    client.write(encodeFrame(Cmd.EXIT));
                });

                let responseData = '';
                let recvBuffer: Buffer = Buffer.alloc(0);
                client.on('data', (data: Buffer) => {
                    recvBuffer = Buffer.concat([recvBuffer, data]);
                    const { frames, rest } = decodeFrames(recvBuffer);
                    recvBuffer = rest;
                    for (const f of frames) responseData += f;
                });
                client.on('end', () => {
                    let returned: number | null = null;
                    if (opts.captureReturned) {
                        const m = responseData.match(/Returned:\s*(-?\d+)/);
                        if (m) returned = parseInt(m[1], 10);
                    }
                    const cleanOutput = responseData.replace(/^RESPONSE: SUCCESS[^\n]*$/gm, '');
                    resolve({ output: cleanOutput.trim(), returned });
                });
                client.on('error', (err) => reject(new Error(`Daemon Connection Error: ${err.message}`)));

            } catch (e: any) {
                reject(new Error(`Pipeline Compilation Error: ${e.message}`));
            }
        });
    }

    private executeRvdlScript(
        sourceCode: string,
        optsOrCallback:
            | { bindMmi?: string; captureReturned?: boolean }
            | ((err: string | null, output: string, returned?: number | null) => void),
        maybeCallback?: (err: string | null, output: string, returned?: number | null) => void,
    ): void {
        const opts = typeof optsOrCallback === 'function' ? {} : optsOrCallback;
        const callback = typeof optsOrCallback === 'function' ? optsOrCallback : maybeCallback!;
        const robustExecute = withRetry(3, { delayMs: 500, backoff: 'exponential' })(
            withTimeout(10_000)((src: string) => this.executeRvdlScriptAsync(src, opts))
        );
        robustExecute(sourceCode).then(
            (r) => callback(null, r.output, r.returned),
            (err) => callback(err instanceof Error ? err.message : String(err), ''),
        );
    }

    private parseJsonBody(req: http.IncomingMessage, res: http.ServerResponse, onParsed: (body: any) => void): void {
        let body = '';
        req.on('data', chunk => { body += chunk.toString(); });
        req.on('end', () => {
            try {
                const parsed = JSON.parse(body || '{}');
                onParsed(parsed);
            } catch (err: any) {
                res.writeHead(400, { 'Content-Type': 'application/json' });
                res.end(JSON.stringify({ error: { message: 'Invalid JSON payload', details: err.message } }));
            }
        });
    }
}