import { test } from 'node:test';
import assert from 'node:assert';
import {
    withTimeout,
    withRetry,
    withFallback,
    withCache,
    withMaxConcurrency,
    AbortError,
    TimeoutError
} from './resilience';

test('withTimeout: resolves before deadline', async () => {
    const fn = withTimeout(100)(async () => 'ok');
    assert.strictEqual(await fn(), 'ok');
});

test('withTimeout: rejects with TimeoutError after deadline', async () => {
    const fn = withTimeout(10)(async () => {
        await new Promise(r => setTimeout(r, 100));
        return 'slow';
    });
    await assert.rejects(fn(), TimeoutError);
});

test('withRetry: retries transient failures then succeeds', async () => {
    let attempts = 0;
    const fn = withRetry(3, { delayMs: 1, backoff: 'linear' })(async () => {
        attempts++;
        if (attempts < 3) throw new Error('transient');
        return 'ok';
    });
    assert.strictEqual(await fn(), 'ok');
    assert.strictEqual(attempts, 3);
});

test('withRetry: never retries AbortError', async () => {
    let attempts = 0;
    const fn = withRetry(3, { delayMs: 1, backoff: 'linear' })(async () => {
        attempts++;
        throw new AbortError();
    });
    await assert.rejects(fn(), AbortError);
    assert.strictEqual(attempts, 1);
});

test('withFallback: returns fallback on failure', async () => {
    const fn = withFallback(() => 'fallback')(async () => {
        throw new Error('boom');
    });
    assert.strictEqual(await fn(), 'fallback');
});

test('withFallback: does not swallow AbortError', async () => {
    const fn = withFallback(() => 'fallback')(async () => {
        throw new AbortError();
    });
    await assert.rejects(fn(), AbortError);
});

test('withCache: deduplicates concurrent identical calls', async () => {
    let calls = 0;
    const fn = withCache(async (x: number) => {
        calls++;
        await new Promise(r => setTimeout(r, 10));
        return x * 2;
    });
    const [a, b] = await Promise.all([fn(1), fn(1)]);
    assert.strictEqual(a, 2);
    assert.strictEqual(b, 2);
    assert.strictEqual(calls, 1);
});

test('withMaxConcurrency: caps concurrent executions', async () => {
    let active = 0;
    let maxActive = 0;
    const fn = withMaxConcurrency(2)(async () => {
        active++;
        maxActive = Math.max(maxActive, active);
        await new Promise(r => setTimeout(r, 10));
        active--;
    });
    await Promise.all([fn(), fn(), fn(), fn(), fn()]);
    assert.ok(maxActive <= 2);
});

test('combinators: nestable (timeout + retry + fallback)', async () => {
    let attempts = 0;
    const composed = withFallback(() => 'gave up')(
        withRetry(2, { delayMs: 1, backoff: 'linear' })(
            withTimeout(50)(async () => {
                attempts++;
                throw new Error('always fails');
            })
        )
    );
    assert.strictEqual(await composed(), 'gave up');
    assert.strictEqual(attempts, 2);
});