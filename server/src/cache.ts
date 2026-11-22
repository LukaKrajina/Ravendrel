import * as fs from 'fs';
import * as path from 'path';
import { createHash } from 'crypto';

export interface CacheDependency {
    path: string;
    mtimeMs: number;
}

export class CompileCache {
    private dir: string;
    private enabled: boolean;







    private compilerStamp: string;

    constructor(dir: string, enabled: boolean = true) {
        this.dir = dir;
        this.enabled = enabled;
        this.compilerStamp = CompileCache.computeCompilerStamp();
    }

    private static computeCompilerStamp(): string {
        const mods = ['cache.js', 'ir.js', 'semantic.js', 'lexer.js', 'parser.js'];
        let newest = 0;
        try {
            const here = path.dirname(__filename);
            for (const m of mods) {
                try {
                    const st = fs.statSync(path.join(here, m));
                    if (st.mtimeMs > newest) newest = st.mtimeMs;
                } catch { }
            }
        } catch { }
        return String(newest);
    }

    private computeKey(srcPath: string, source: string, deps: CacheDependency[]): string {
        const h = createHash('sha256');
        h.update(srcPath);
        h.update('\u0000');
        h.update(this.compilerStamp);
        h.update('\u0000');
        h.update(source);
        for (const d of deps) {
            h.update('\u0000');
            h.update(d.path);
            h.update(':');
            h.update(String(d.mtimeMs));
        }
        return h.digest('hex');
    }

    get(srcPath: string, source: string, deps: CacheDependency[]): string | null {
        if (!this.enabled) return null;
        const f = path.join(this.dir, this.computeKey(srcPath, source, deps) + '.ir');
        try {
            if (fs.existsSync(f)) return fs.readFileSync(f, 'utf-8');
        } catch {
        }
        return null;
    }

    set(srcPath: string, source: string, deps: CacheDependency[], ir: string): void {
        if (!this.enabled) return;
        try {
            fs.mkdirSync(this.dir, { recursive: true });
            fs.writeFileSync(path.join(this.dir, this.computeKey(srcPath, source, deps) + '.ir'), ir);
        } catch {
        }
    }

    clear(): void {
        try {
            if (fs.existsSync(this.dir)) {
                for (const f of fs.readdirSync(this.dir)) {
                    fs.unlinkSync(path.join(this.dir, f));
                }
            }
        } catch {
        }
    }
}