import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer } from './lexer';
import { Parser } from './parser';
import { buildMir } from './mir';
import { serializeMir, deserializeMir, MIR_FORMAT_VERSION } from './mir-serialize';
import { Cmd } from './protocol';

function buildProgram(src: string) {
    const ast = new Parser(new Lexer(src)).parse();
    return buildMir(ast);
}

test('mir-serialize: round-trip preserves MirProgram', () => {
    const prog = buildProgram(
        'int32 rvdl_main() { int32 x = 1; int32 r = 0; ' +
        'while (x < 3) { x = x + 1; if (x == 2) { r = r + 1; } } return r; }');
    const json = serializeMir(prog);
    const restored = deserializeMir(json);
    assert.deepStrictEqual(restored, prog, 'round-trip is lossless');
});

test('mir-serialize: serialized payload carries version', () => {
    const prog = buildProgram('int32 rvdl_main() { return 0; }');
    const parsed = JSON.parse(serializeMir(prog));
    assert.strictEqual(parsed.version, MIR_FORMAT_VERSION);
    assert.ok(Array.isArray(parsed.bodies));
});

test('mir-serialize: wrong version is rejected', () => {
    assert.throws(() => deserializeMir(JSON.stringify({ version: 999, bodies: [] })), /version/);
});

test('mir-serialize: missing bodies is rejected', () => {
    assert.throws(() => deserializeMir(JSON.stringify({ version: MIR_FORMAT_VERSION })), /bodies/);
});

test('mir-serialize: malformed body is rejected', () => {
    assert.throws(
        () => deserializeMir(JSON.stringify({ version: MIR_FORMAT_VERSION, bodies: [{ blocks: [] }] })),
        /owner/);
});

test('protocol: COMPILE_MIR command is defined', () => {
    assert.strictEqual(Cmd.COMPILE_MIR, 0x0d);
});
