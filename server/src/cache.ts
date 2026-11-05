// ============================================================================
// 编译产物缓存（增量编译）
//
// 以「源内容 SHA-256 + 各 import 依赖（.rvdli）的 mtime」为 key，把编译出的
// LLVM IR 持久化到磁盘。命中则跳过 lexer/parser/semantic/IR 全链路，仅在大
// 项目（多 include / 多 import）下带来可观的编译提速。
// ============================================================================

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
    /**
     * 编译器自身指纹：codegen 相关模块（ir/semantic/lexer/parser）与本文件的
     * mtime 取最大值。写进缓存 key，使得**编译器升级后旧 IR 自动失效**。
     *
     * 此前 key 只含「源内容 + 依赖 mtime」，编译器改动不会让缓存失效 ——
     * 下游工程（如 QCOS 内核）在修完编译器后仍会拿到旧 IR，表现为
     * 「明明改了却还是旧的错误」，极难定位。
     */
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
                } catch { /* 模块不存在则跳过 */ }
            }
        } catch { /* 取不到就用 0 */ }
        return String(newest);
    }

    /** 缓存 key = hash(srcPath ‖ source ‖ dep0:path:mt ‖ dep1:... ) */
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
            /* 缓存读取失败视为未命中 */
        }
        return null;
    }

    set(srcPath: string, source: string, deps: CacheDependency[], ir: string): void {
        if (!this.enabled) return;
        try {
            fs.mkdirSync(this.dir, { recursive: true });
            fs.writeFileSync(path.join(this.dir, this.computeKey(srcPath, source, deps) + '.ir'), ir);
        } catch {
            /* 缓存写入失败不影响编译 */
        }
    }

    /** 清空缓存目录 */
    clear(): void {
        try {
            if (fs.existsSync(this.dir)) {
                for (const f of fs.readdirSync(this.dir)) {
                    fs.unlinkSync(path.join(this.dir, f));
                }
            }
        } catch {
            /* ignore */
        }
    }
}
