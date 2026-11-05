import { test } from 'node:test';
import assert from 'node:assert';
import { Lexer } from './lexer';
import { Parser, LAYER_TAG_SCALAR_KEYS, LAYER_TAG_LIST_KEYS } from './parser';
import { buildTopology } from './topology';
import {
    LAYER_TAG_KEY_NAMES, LAYER_TAG_INSERTS, LAYER_KEY_DOCS,
    LAYER_ENUM_VALUES, layerKeyDoc, layerEnumValueDoc,
} from './hover-docs';
import { insideLayerTagKeys, enclosingLayerTag, layerKeyBefore } from './layer-context';

function parse(src: string) {
    return new Parser(new Lexer(src)).parse();
}
function topo(src: string) {
    return buildTopology(parse(src));
}

// ═══════════════════════════════════════════════════════════════════
// 一、编辑器键表 ↔ 编译器键表：任何一侧新增都必须同时更新另一侧
// ═══════════════════════════════════════════════════════════════════

test('键表一致: 编辑器键名集合 == 编译器键全集（标量 ∪ 列表）', () => {
    const compilerKeys = new Set<string>([...LAYER_TAG_SCALAR_KEYS, ...LAYER_TAG_LIST_KEYS]);
    const editorKeys = new Set<string>(LAYER_TAG_KEY_NAMES);
    assert.deepStrictEqual(
        [...editorKeys].sort(), [...compilerKeys].sort(),
        '编辑器与编译器的 @layer 键表漂移：'
        + '编辑器多出的键会「补得出来但报 E-TOP008」，少掉的键会「写得对但无补全」');
});

test('键表一致: 每个编辑器键都有文档与插入文本（无遗漏）', () => {
    for (const k of LAYER_TAG_KEY_NAMES) {
        assert.ok(layerKeyDoc(k), `键 '${k}' 缺文档`);
        assert.ok(LAYER_TAG_INSERTS[k], `键 '${k}' 缺插入文本`);
        assert.ok(LAYER_KEY_DOCS[k].purpose.length > 0, `键 '${k}' 的 purpose 为空`);
        assert.ok(LAYER_KEY_DOCS[k].purposeEn.length > 0, `键 '${k}' 的 purposeEn 为空`);
    }
});

test('键表一致: 键表无重复项', () => {
    assert.strictEqual(new Set(LAYER_TAG_KEY_NAMES).size, LAYER_TAG_KEY_NAMES.length);
});

test('键表一致: 每个键都真的被编译器接受（端到端）', () => {
    // 用每个键各写一遍完整的 @layer，断言不产生 E-TOP008。
    // 这比比对集合更强：它证明键在真实解析路径上被消费，而非仅在表里。
    const tag =
        'time=0, thread=0, coord=(0), cost=1, deadline=9, dual=1, ' +
        'shuttle=(1), shuttle_lambda=1, shuttle_phase=2, shuttle_rounds=1, shuttle_strategy=-1, ' +
        'track=(expect_z), track_mode=0, track_slots=1';
    const codes = topo(`@layer(${tag})\nint32 f() { return 0; }\n@layer(time=1, thread=0, coord=(1))\nint32 g0() { return 1; }`)
        .errors.map(e => e.code);
    assert.ok(!codes.includes('E-TOP008'), `键全集应全部被接受；实际 ${JSON.stringify(codes)}`);
});

// ═══════════════════════════════════════════════════════════════════
// 二、键位判定（决定补全弹什么）
// ═══════════════════════════════════════════════════════════════════

test('键位: @layer( 之后是键位', () => {
    assert.ok(insideLayerTagKeys('@layer('));
    assert.ok(insideLayerTagKeys('@layer(time=0, '));
    assert.ok(insideLayerTagKeys('@layer(time=0, thread=0, coord=(0,0), '));
    assert.ok(insideLayerTagKeys('@layer(\n    time=0,\n    '));
});

test('键位: coord=(...) / track=(...) / shuttle=(...) 内部**不是**键位', () => {
    // 在这些位置弹出的应是数字/标识符，弹键名会误导用户写出 coord=(0, time=1)
    assert.ok(!insideLayerTagKeys('@layer(time=0, coord=('));
    assert.ok(!insideLayerTagKeys('@layer(time=0, coord=(0,'));
    assert.ok(!insideLayerTagKeys('@layer(time=0, track=(expect_z,'));
    assert.ok(!insideLayerTagKeys('@layer(time=0, shuttle=(1,'));
});

test('键位: 标签已闭合后不是键位', () => {
    assert.ok(!insideLayerTagKeys('@layer(time=0, thread=0)\nint32 f() { '));
    assert.ok(insideLayerTagKeys('@layer(time=0)\n@layer(time=1, '), '最后一个 @layer 未闭合 → 是键位');
});

test('键位: 完全不在 @layer 内时不是键位', () => {
    assert.ok(!insideLayerTagKeys(''));
    assert.ok(!insideLayerTagKeys('int32 f() { '));
    assert.ok(!insideLayerTagKeys('@[gate]\nvoid U(Qubit q) { '));
});

test('键位: 取值位置（`time=` 之后）不是键位', () => {
    assert.ok(!insideLayerTagKeys('@layer(time='));
    assert.ok(!insideLayerTagKeys('@layer(time=0, coord='));
});

// ═══════════════════════════════════════════════════════════════════
// 三、包围标签判定（决定悬停是否走 @layer 键文档）
// ═══════════════════════════════════════════════════════════════════

test('包围: 键名位置落在 @layer 内', () => {
    const text = '@layer(time=0, shuttle_lambda=1)';
    const i = text.indexOf('shuttle_lambda');
    assert.strictEqual(enclosingLayerTag(text, i), 0);
});

test('包围: 标签在文件中部时返回正确起始下标', () => {
    const pre = '// header\n@layer(time=0, track=(expect_z))\nint32 f() { return 0; }';
    const i = pre.indexOf('track');
    assert.strictEqual(enclosingLayerTag(pre, i), pre.indexOf('@layer'));
});

test('包围: 位于 coord=(...) 内时仍识别为 @layer 内（嵌套括号）', () => {
    const text = '@layer(time=0, coord=(0,0))';
    const i = text.indexOf('0,0');
    const at = enclosingLayerTag(text, i);
    assert.strictEqual(at, 0, '嵌套括号不应让判定逃出 @layer');
});

test('包围: @layer 之外的标识符不被误判', () => {
    assert.strictEqual(enclosingLayerTag('int32 time = 0;', 6), null);
    assert.strictEqual(enclosingLayerTag('f(time, coord)', 2), null);
    assert.strictEqual(enclosingLayerTag('', 0), null);
});

test('包围: 闭合的 @layer(...) 之后的标识符不被误判', () => {
    const text = '@layer(time=0)\nint32 f() { return 0; }';
    assert.strictEqual(enclosingLayerTag(text, text.indexOf('f')), null);
});

test('包围: 多个 @layer 时归入正确的那一个', () => {
    const text = '@layer(time=0)\nint32 a() { return 0; }\n@layer(time=1, shuttle=(2))\nint32 b() { return 1; }';
    const i = text.indexOf('shuttle');
    assert.strictEqual(enclosingLayerTag(text, i), text.lastIndexOf('@layer'));
});

// ═══════════════════════════════════════════════════════════════════
// 四、键值定位（决定枚举取值悬停）
// ═══════════════════════════════════════════════════════════════════

test('键值: 取 `key=value` 中的 key', () => {
    assert.strictEqual(layerKeyBefore('@layer(shuttle_strategy=3', 24), 'shuttle_strategy');
    assert.strictEqual(layerKeyBefore('@layer(shuttle_strategy = 3', 26), 'shuttle_strategy');
    assert.strictEqual(layerKeyBefore('@layer(time=0, track_mode=1', 26), 'track_mode');
});

test('键值: 非 `key=` 位置返回 null', () => {
    // `@layer(shuttle=(1` —— 光标在 `1` 上，其前面是 `(` 而非 `=`
    const s = '@layer(shuttle=(1';
    assert.strictEqual(layerKeyBefore(s, s.indexOf('1')), null);
    assert.strictEqual(layerKeyBefore('@layer(1', 7), null);
    assert.strictEqual(layerKeyBefore('3', 0), null);
});

// ═══════════════════════════════════════════════════════════════════
// 五、枚举取值文档：取值域必须与编译器实际校验的一致
// ═══════════════════════════════════════════════════════════════════

test('枚举文档: 取值集合与编译器接受域一致（shuttle_phase）', () => {
    assert.deepStrictEqual(Object.keys(LAYER_ENUM_VALUES.shuttle_phase).sort(), ['0', '1', '2']);
    for (const v of [0, 1, 2]) {
        assert.deepStrictEqual(
            topo(`@layer(time=0, thread=0, coord=(0), shuttle=(1), shuttle_phase=${v})\nint32 f() { return 0; }\n@layer(time=1, thread=0, coord=(1))\nint32 g0() { return 1; }`)
                .errors.filter(e => e.code === 'E-SHU004'), [],
            `phase=${v} 应被接受（文档列出了它）`);
    }
    assert.ok(topo(`@layer(time=0, thread=0, coord=(0), shuttle=(1), shuttle_phase=3)\nint32 f() { return 0; }\n@layer(time=1, thread=0, coord=(1))\nint32 g0() { return 1; }`)
        .errors.some(e => e.code === 'E-SHU004'), 'phase=3 应被拒（文档未列出它）');
});

test('枚举文档: 取值集合与编译器接受域一致（shuttle_strategy）', () => {
    assert.deepStrictEqual(Object.keys(LAYER_ENUM_VALUES.shuttle_strategy).sort(),
        ['-1', '0', '1', '2', '3', '4']);
    for (const v of [-1, 0, 1, 2, 3, 4]) {
        assert.deepStrictEqual(
            topo(`@layer(time=0, thread=0, coord=(0), shuttle=(1), shuttle_strategy=${v})\nint32 f() { return 0; }\n@layer(time=1, thread=0, coord=(1))\nint32 g0() { return 1; }`)
                .errors.filter(e => e.code === 'E-SHU005'), [],
            `strategy=${v} 应被接受（文档列出了它）`);
    }
    assert.ok(topo(`@layer(time=0, thread=0, coord=(0), shuttle=(1), shuttle_strategy=5)\nint32 f() { return 0; }\n@layer(time=1, thread=0, coord=(1))\nint32 g0() { return 1; }`)
        .errors.some(e => e.code === 'E-SHU005'), 'strategy=5 应被拒（文档未列出它）');
});

test('枚举文档: 取值集合与编译器接受域一致（track_mode）', () => {
    assert.deepStrictEqual(Object.keys(LAYER_ENUM_VALUES.track_mode).sort(), ['0', '1', '2', '3']);
    for (const v of [0, 1, 2, 3]) {
        assert.deepStrictEqual(
            topo(`@layer(time=0, thread=0, coord=(0), track=(expect_z), track_mode=${v})\nint32 f() { return 0; }`)
                .errors.filter(e => e.code === 'E-TRK004'), [],
            `mode=${v} 应被接受（文档列出了它）`);
    }
    assert.ok(topo('@layer(time=0, thread=0, coord=(0), track=(expect_z), track_mode=4)\nint32 f() { return 0; }')
        .errors.some(e => e.code === 'E-TRK004'), 'mode=4 应被拒（文档未列出它）');
});

test('枚举文档: 每个枚举键都在编辑器键表内（否则悬停永不触发）', () => {
    for (const k of Object.keys(LAYER_ENUM_VALUES)) {
        assert.ok((LAYER_TAG_KEY_NAMES as readonly string[]).includes(k),
            `枚举键 '${k}' 不在 LAYER_TAG_KEY_NAMES 中，其取值悬停将永不触发`);
        assert.ok(layerKeyDoc(k), `枚举键 '${k}' 缺键文档`);
    }
});

test('枚举文档: 说明双语齐备且非空', () => {
    for (const [key, vals] of Object.entries(LAYER_ENUM_VALUES)) {
        for (const [v, doc] of Object.entries(vals)) {
            assert.ok(doc.zh.length > 0, `${key}=${v} 缺中文说明`);
            assert.ok(doc.en.length > 0, `${key}=${v} 缺英文说明`);
        }
    }
});

test('枚举文档: 安全索引对非枚举键返回 undefined（不抛异常）', () => {
    assert.strictEqual(layerEnumValueDoc('time', '0'), undefined);
    assert.strictEqual(layerEnumValueDoc('nonexistent', '0'), undefined);
    assert.strictEqual(layerEnumValueDoc('track_mode', '99'), undefined);
    assert.ok(layerEnumValueDoc('track_mode', '3'));
});

// ═══════════════════════════════════════════════════════════════════
// 六、安全索引
// ═══════════════════════════════════════════════════════════════════

test('安全索引: 非键名返回 undefined（含原型链属性名）', () => {
    assert.strictEqual(layerKeyDoc('nonexistent'), undefined);
    assert.strictEqual(layerKeyDoc('toString'), undefined, '原型链属性不得被当作键文档');
    assert.strictEqual(layerKeyDoc('__proto__'), undefined);
    assert.ok(layerKeyDoc('shuttle_strategy'));
    assert.ok(layerKeyDoc('track_slots'));
});

test('插入文本: 需要开括号的键以 `(` 结尾', () => {
    const BRACKET_KEYS = ['coord', 'shuttle', 'track'] as const;
    for (const k of BRACKET_KEYS) {
        assert.ok(LAYER_TAG_INSERTS[k].endsWith('('), `'${k}' 的插入文本应带开括号`);
    }
    for (const k of LAYER_TAG_KEY_NAMES) {
        if ((BRACKET_KEYS as readonly string[]).includes(k)) continue;
        assert.ok(LAYER_TAG_INSERTS[k].endsWith('='), `'${k}' 的插入文本应以 = 结尾`);
    }
});

test('插入文本: 补全后逐键形成合法 @layer（端到端）', () => {
    // 模拟补全实际做的事：在键位插入文本 + 补一个示例取值 + 闭合括号。
    // 目标是证明插入文本在语法上可用，而不是让用户补出一个必然报错的键。
    const SAMPLE: Record<string, string> = {
        coord: '0', shuttle: '1', track: 'expect_z',
        shuttle_lambda: '1', shuttle_phase: '2', shuttle_rounds: '1',
        shuttle_strategy: '-1', track_mode: '0', track_slots: '1',
    };
    for (const k of LAYER_TAG_KEY_NAMES) {
        const ins = LAYER_TAG_INSERTS[k];
        const val = SAMPLE[k] ?? '0';
        const closes = ins.endsWith('(') ? ')' : '';
        const tag = `${ins}${val}${closes}`;
        const codes = topo(`@layer(${tag})\nint32 f() { return 0; }`).errors.map(e => e.code);
        assert.ok(!codes.includes('E-TOP008'),
            `键 '${k}' 的插入文本 '${ins}' 生成未知键（编译器不认识它）：${JSON.stringify(codes)}`);
        assert.doesNotThrow(() => topo(`@layer(${tag})\nint32 f() { return 0; }`),
            `键 '${k}' 的插入文本 '${ins}' 不应导致解析崩溃`);
    }
});
