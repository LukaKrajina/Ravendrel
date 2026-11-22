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
                    if (e instanceof AbortError) throw e;
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