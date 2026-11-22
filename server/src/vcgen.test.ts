import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { VCGenerator } from './vcgen';

function obligations(src: string) {
    const parser = new Parser(new Lexer(src));
    const ast = parser.parse();
    return new VCGenerator().generate(ast);
}

test('vcgen: function with contracts generates obligations', () => {
    const obs = obligations(`
        int32 abs(int32 x) requires x >= 0 ensures result >= 0 {
            return x;
        }
    `);
    assert.ok(obs.length > 0);
    assert.ok(obs.some(o => o.id.includes('abs')));
});

test('vcgen: no contracts produces no obligations', () => {
    const obs = obligations('int32 f() { return 0; }');
    assert.strictEqual(obs.length, 0);
});

test('vcgen: while with invariant produces obligations', () => {
    const obs = obligations(`
        int32 loop(int32 n) ensures result >= 0 {
            int32 i = 0;
            while (i < n) invariant i >= 0 {
                i = i + 1;
            }
            return i;
        }
    `);
    assert.ok(obs.some(o => o.id.includes('while')));
});

test('vcgen: for loop with invariant produces obligations', () => {
    const obs = obligations(`
        int32 sum(int32 n) ensures result >= 0 {
            int32 total = 0;
            for (int32 i = 0; i < n; i = i + 1) invariant total >= 0 {
                total = total + i;
            }
            return total;
        }
    `);
    assert.ok(obs.some(o => o.id.includes('for')));
});

test('vcgen: toProtocol emits OBLIGATION/ANTE/CONSE lines', () => {
    const obs = obligations('int32 f() requires true ensures true { return 0; }');
    const protocol = new VCGenerator().toProtocol(obs);
    assert.ok(protocol.includes('OBLIGATION'));
    assert.ok(protocol.includes('ANTE'));
    assert.ok(protocol.includes('CONSE'));
});

test('vcgen: toSmtLib emits check-sat', () => {
    const obs = obligations('int32 f() requires true ensures true { return 0; }');
    const smt = new VCGenerator().toSmtLib(obs);
    assert.ok(smt.includes('(check-sat)'));
});