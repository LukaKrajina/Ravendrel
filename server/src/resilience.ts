// ============================================================================
// 可组合的弹性异步组合子
// 把每个弹性策略建模为「类型闭合」的高阶组合子：
//   combinator: (fn: (...A) => Promise<R>) => (...A) => Promise<R>
// 输入输出类型完全相同，因此可像套娃一样嵌套组合：
//   withCache(withRetry(withRateLimit(withTimeout(fn, 10s), 200), 3, ...))
//
// 泛型设计：组合子工厂（withTimeout / withRetry / ...）返回一个
// 保持签名的包装器 <A, R>(fn) => fn，泛型在「应用 fn」时才推断，
// 从而支持任意深度的嵌套组合而不丢失类型。
// ============================================================================

export class AbortError extends Error {
    constructor(message = 'Operation aborted') {
        super(message);
        this.name = 'AbortError';
    }
}

export class TimeoutError extends Error {
    constructor(message = 'Operation timed out') {
        super(message);
        this.name = 'TimeoutError';
    }
}

export type AsyncFn<A extends unknown[], R> = (...args: A) => Promise<R>;

function sleep(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

// ---------------------------------------------------------------------------
// withTimeout：为调用设置截止时间，超时抛 TimeoutError（可重试的普通错误）
// ---------------------------------------------------------------------------
export function withTimeout(ms: number) {
    return <A extends unknown[], R>(fn: AsyncFn<A, R>): AsyncFn<A, R> => {
        return async (...args: A): Promise<R> => {
            return new Promise<R>((resolve, reject) => {
                const timer = setTimeout(() => {
                    reject(new TimeoutError(`timed out after ${ms}ms`));
                }, ms);
                fn(...args).then(
                    (v) => { clearTimeout(timer); resolve(v); },
                    (e) => { clearTimeout(timer); reject(e); }
                );
            });
        };
    };
}

// ---------------------------------------------------------------------------
// withRetry：失败自动重试，指数/线性退避；AbortError 绝不重试
// ---------------------------------------------------------------------------
export interface RetryOptions {
    delayMs: number;
    backoff: 'exponential' | 'linear';
    maxDelayMs?: number;
}

export function withRetry(maxAttempts: number, opts: RetryOptions) {
    return <A extends unknown[], R>(fn: AsyncFn<A, R>): AsyncFn<A, R> => {
        return async (...args: A): Promise<R> => {
            let attempt = 0;
            let delay = opts.delayMs;
            for (;;) {
                attempt++;
                try {
                    return await fn(...args);
                } catch (e) {
                    if (e instanceof AbortError) throw e; // 取消绝不重试
                    if (attempt >= maxAttempts) throw e;
                    await sleep(delay);
                    if (opts.backoff === 'exponential') {
                        delay = Math.min(delay * 2, opts.maxDelayMs ?? Infinity);
                    }
                }
            }
        };
    };
}

// ---------------------------------------------------------------------------
// withFallback：失败后返回兜底值；AbortError 绝不兜底
// ---------------------------------------------------------------------------
export function withFallback<R>(fallback: () => R) {
    return <A extends unknown[]>(fn: AsyncFn<A, R>): AsyncFn<A, R> => {
        return async (...args: A): Promise<R> => {
            try {
                return await fn(...args);
            } catch (e) {
                if (e instanceof AbortError) throw e;
                return fallback();
            }
        };
    };
}

// ---------------------------------------------------------------------------
// withCache：缓存 Promise 本身（而非解析值），合并并发的重复调用
// 失败（含取消）后清除缓存条目，保证取消不会被缓存。
// ---------------------------------------------------------------------------
export function withCache<A extends unknown[], R>(fn: AsyncFn<A, R>): AsyncFn<A, R> {
    const cache = new Map<string, Promise<R>>();
    return async (...args: A): Promise<R> => {
        const key = JSON.stringify(args);
        const hit = cache.get(key);
        if (hit) return hit;
        const p = fn(...args);
        cache.set(key, p);
        p.catch(() => { cache.delete(key); });
        return p;
    };
}

// ---------------------------------------------------------------------------
// withMaxConcurrency：限制同时运行的数量（信号量）
// ---------------------------------------------------------------------------
export function withMaxConcurrency(limit: number) {
    return <A extends unknown[], R>(fn: AsyncFn<A, R>): AsyncFn<A, R> => {
        let active = 0;
        const queue: (() => void)[] = [];
        const next = () => {
            active--;
            const w = queue.shift();
            if (w) { active++; w(); }
        };
        return async (...args: A): Promise<R> => {
            if (active >= limit) {
                await new Promise<void>((resolve) => queue.push(resolve));
            }
            active++;
            try {
                return await fn(...args);
            } finally {
                next();
            }
        };
    };
}