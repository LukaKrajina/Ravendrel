import { test } from 'node:test';
import assert from 'node:assert';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { LAYER_TAG_SCALAR_KEYS, LAYER_TAG_LIST_KEYS } from './parser';
import { LAYER_ENUM_VALUES, LAYER_TAG_KEY_NAMES } from './hover-docs';

// ═══════════════════════════════════════════════════════════════════
// 语法着色（syntaxes/rvdl.tmLanguage.json）与编译器键表的一致性守卫
//
// 为什么需要它：tmLanguage 是纯文本正则，没有任何环节会检查它「认不认识」
// 编译器新加的键。漏掉的后果是**静默**的 —— 代码能编译、能运行，
// 只是那个键在编辑器里褪成无色，用户以为写错了。
// 反过来的后果更糟：着色把一个编译器会拒的键染成合法键的颜色。
//
// 因此本文件的判据不是「正则能跑通」，而是：
//   着色表认定的「列表键 / 枚举键 / 标量键」三分 ==
//   编译器与文档认定的同一三分。
// ═══════════════════════════════════════════════════════════════════

const GRAMMAR_PATH = join(__dirname, '..', '..', 'syntaxes', 'rvdl.tmLanguage.json');
const grammar: any = JSON.parse(readFileSync(GRAMMAR_PATH, 'utf8'));
const repo: Record<string, any> = grammar.repository;

interface Leaf { ruleName: string; rule: any; }

/** 把 `#include` 链展开成有序的叶子规则表（只有 match / begin+end 是叶子）。 */
function leafRules(name: string, seen: string[] = []): Leaf[] {
    assert.ok(!seen.includes(name), `#${name} 的 include 成环：${seen.join(' → ')} → ${name}`);
    const r = repo[name];
    assert.ok(r, `规则 #${name} 不存在`);
    if (r.match || (r.begin && r.end)) return [{ ruleName: name, rule: r }];
    return (r.patterns ?? []).flatMap((p: any) => {
        if (p.include) return leafRules(p.include.slice(1), [...seen, name]);
        // 内联子规则（如 #strings 里的双引号/单引号），规则名沿用容器名
        if (p.match || (p.begin && p.end)) return [{ ruleName: name, rule: p }];
        return [];
    });
}

/**
 * `#layer-tag` **内容区**的叶子规则表。
 *
 * 注意不能对 `layer-tag` 直接调 leafRules：它自身就是 begin/end 规则，
 * 那样会把整个区域当成一个叶子，于是 begin `(@layer)\s*(\()` 被拿去匹配
 * 内容（内容里没有 `@layer(`）→ 一条都不命中 → 扫描静默退化成空转。
 */
function layerTagContentRules(): Leaf[] {
    return repo['layer-tag'].patterns.flatMap((p: any) =>
        p.include ? leafRules(p.include.slice(1)) : []);
}

/** sticky 正则：等价于「从 pos 处锚定匹配」，即 TextMate 的逐位尝试。 */
function stickyAt(re: string, pos: number, s: string): RegExpExecArray | null {
    const r = new RegExp(re, 'y');
    r.lastIndex = pos;
    return r.exec(s);
}

interface Event { ruleName: string; start: number; end: number; text: string; cap1: string; }

/**
 * 极简 TextMate 扫描器：在 `body` 上按规则顺序逐位尝试，返回实际命中的片段。
 * `base` 为本段在整行中的绝对起点（用于把嵌套事件的坐标还原成绝对坐标）。
 *
 * 只实现本语法用到的一档嵌套（begin/end 内部不再嵌套 begin/end），
 * 但足以验证最容易出错的一点：列表取值 `coord=(0,0)` 的闭合括号
 * 不会被外层 `#layer-tag` 的 end 抢走；同时也会深入到列表内部，
 * 记录可观测量 / 数字各自的片段。
 */
function scan(body: string, leaves: Leaf[], base: number): Event[] {
    const events: Event[] = [];
    let pos = 0;
    while (pos < body.length) {
        let advanced = false;
        for (const { ruleName, rule } of leaves) {
            if (rule.match) {
                const m = stickyAt(rule.match, pos, body);
                if (!m) continue;
                events.push({
                    ruleName, start: base + pos, end: base + pos + m[0].length,
                    text: m[0], cap1: m[1] ?? '',
                });
                pos += m[0].length; advanced = true; break;
            }
            const b = stickyAt(rule.begin, pos, body);
            if (!b) continue;
            // 找本规则的 end：逐位前进（本语法的列表取值内部无嵌套括号）
            let q = pos + b[0].length, endIdx = -1, endLen = 0;
            while (q <= body.length) {
                const e = stickyAt(rule.end, q, body);
                if (e) { endIdx = q + e[0].length; endLen = e[0].length; break; }
                q++;
            }
            assert.ok(endIdx >= 0,
                `规则 #${ruleName} 的 begin 在此处没有配对 end：${body.slice(pos, pos + 20)}`);
            events.push({
                ruleName, start: base + pos, end: base + endIdx,
                text: body.slice(pos, endIdx), cap1: b[1] ?? '',
            });
            // 深入区域内部：列表取值里的可观测量/数字、字符串里的转义等
            const inner = body.slice(pos + b[0].length, endIdx - endLen);
            if (inner.length > 0) {
                events.push(...scan(inner, inlineRules(ruleName, rule), base + pos + b[0].length));
            }
            pos = endIdx; advanced = true; break;
        }
        if (!advanced) pos++;   // 空白 / 逗号：无规则命中，跳过
    }
    return events;
}

/** 展开某个 begin/end 规则自身的 content patterns（支持内联与 #include 混用）。 */
function inlineRules(container: string, rule: any): Leaf[] {
    return (rule.patterns ?? []).flatMap((p: any) => {
        if (p.include) return leafRules(p.include.slice(1));
        if (p.match || (p.begin && p.end)) return [{ ruleName: container, rule: p }];
        return [];
    });
}

function scanLayerTagBody(body: string): { events: Event[]; covered: boolean } {
    const leaves = layerTagContentRules();
    const events = scan(body, leaves, 0);
    const covered = events.length > 0
        && Math.max(...events.map(e => e.end)) >= body.replace(/\s+$/, '').length;
    // 未命中任何规则的位置视为「已跳过」，因此 covered 用「最末事件是否触底」判定
    return { events, covered };
}

/** 完整键全集的 `@layer(...)` 内容（每键各出现一次，取值取合法样本） */
const CANONICAL_BODY =
    'time=0, thread=0, coord=(0,0), cost=1, deadline=9, dual=1, ' +
    'shuttle=(1,0), shuttle_lambda=1, shuttle_phase=2, shuttle_rounds=1, shuttle_strategy=-1, ' +
    'track=(expect_z, entropy), track_mode=0, track_slots=1';

// ═══════════════════════════════════════════════════════════════════
// 一、结构完整性
// ═══════════════════════════════════════════════════════════════════

test('语法: 文件是合法 JSON 且包含必需顶层键', () => {
    assert.strictEqual(grammar.scopeName, 'source.ravendrel');
    assert.ok(Array.isArray(grammar.patterns));
    assert.ok(repo, 'repository 缺失');
});

test('语法: 所有 #include 目标都存在（悬空 include 会让整段着色失效）', () => {
    const dangling: string[] = [];
    const visit = (o: any, where: string) => {
        if (Array.isArray(o)) { o.forEach((v, i) => visit(v, `${where}[${i}]`)); return; }
        if (!o || typeof o !== 'object') return;
        if (typeof o.include === 'string') {
            const name = o.include.replace(/^#/, '');
            if (!repo[name]) dangling.push(`${where} → #${name}`);
        }
        for (const [k, v] of Object.entries(o)) visit(v, `${where}.${k}`);
    };
    visit(grammar, '$');
    assert.deepStrictEqual(dangling, [], `悬空 include：${dangling.join(', ')}`);
});

test('语法: annotations 先试 @layer 区域规则，再回退纯关键字', () => {
    const incs = repo.annotations.patterns.map((p: any) => p.include);
    assert.strictEqual(incs[0], '#layer-tag',
        '#layer-tag 必须排在回退规则之前，否则整个标签只被染成一个关键字色');
    // 回退规则存在的理由：用户还没敲 `(` 时也要有高亮
    assert.ok(repo.annotations.patterns.some((p: any) => p.name === 'keyword.declaration.ravendrel' && p.match === '@layer\\b'));
});

test('语法: @layer 区域用 begin/end（而非单条 match）以容纳嵌套括号', () => {
    assert.ok(!repo['layer-tag'].match, '@layer 不能用单条 match —— 取值里有嵌套括号');
    assert.strictEqual(repo['layer-tag'].begin, '(@layer)\\s*(\\()');
    assert.strictEqual(repo['layer-tag'].end, '\\)');
    assert.strictEqual(repo['layer-tag'].contentName, 'meta.layer.ravendrel');
});

// ═══════════════════════════════════════════════════════════════════
// 二、三分一致：着色表 == 编译器 ∪ 文档
// ═══════════════════════════════════════════════════════════════════

/** 取规则第 n 个捕获组里的 `(a|b|c)` 键名列表 */
function alternationKeys(re: string, group: number): string[] {
    // 逐层扫描括号，找到第 group 个 '(' 的内容
    let depth = 0, start = -1;
    for (let i = 0; i < re.length; i++) {
        if (re[i] === '\\') { i++; continue; }
        if (re[i] === '(') {
            depth++;
            if (depth === group) { start = i + 1; }
            else if (depth > group) continue;
        } else if (re[i] === ')') {
            if (depth === group && start >= 0) return re.slice(start, i).split('|');
            depth--;
        }
    }
    throw new Error(`正则 ${re} 没有第 ${group} 个捕获组`);
}

test('三分一致: #layer-list-value 的键 == 编译器的列表键', () => {
    assert.deepStrictEqual(
        alternationKeys(repo['layer-list-value'].begin, 1).sort(),
        [...LAYER_TAG_LIST_KEYS].sort(),
        '列表键漂移：编译器接受的列表键与着色表不一致');
});

test('三分一致: #layer-enum-value 的键 == 有取值域的键（文档枚举键）', () => {
    assert.deepStrictEqual(
        alternationKeys(repo['layer-enum-value'].match, 1).sort(),
        Object.keys(LAYER_ENUM_VALUES).sort(),
        '枚举键漂移：文档给出了取值域、着色表却不认识它');
});

test('三分一致: #layer-scalar-key 的键 == 标量键 − 枚举键', () => {
    const expected = [...LAYER_TAG_SCALAR_KEYS].filter(k => !(k in LAYER_ENUM_VALUES)).sort();
    assert.deepStrictEqual(
        alternationKeys(repo['layer-scalar-key'].match, 1).sort(), expected,
        '标量键漂移：漏掉的键会褪成无色，多出的键会把编译器拒的键染成合法色');
});

test('三分一致: 三条键规则两两不交，并集 == 编辑器键全集', () => {
    const list = alternationKeys(repo['layer-list-value'].begin, 1);
    const enumK = alternationKeys(repo['layer-enum-value'].match, 1);
    const scalar = alternationKeys(repo['layer-scalar-key'].match, 1);

    // 枚举键也写成 `key=N`，因此它在**语法**上属于标量键，但着色上必须由
    // #layer-enum-value 独占 —— 若同时出现在 scalar 的候选里，（多出的重复
    // 反而不可见，但）规则顺序一变就会静默降级为普通数字色。
    const dupe = (a: string[], b: string[]) => a.filter(k => b.includes(k));
    assert.deepStrictEqual(dupe(list, scalar), [], '列表键与标量键重叠');
    assert.deepStrictEqual(dupe(list, enumK), [], '列表键与枚举键重叠');
    assert.deepStrictEqual(dupe(enumK, scalar), [], '枚举键不得同时出现在两条规则里');

    // 枚举键是标量键的子集（编译器侧的事实），由编译器键表保证
    for (const k of enumK) {
        assert.ok(LAYER_TAG_SCALAR_KEYS.has(k), `枚举键 '${k}' 不在编译器标量键表内`);
    }

    assert.deepStrictEqual(
        [...list, ...enumK, ...scalar].sort(), [...LAYER_TAG_KEY_NAMES].sort(),
        '着色表覆盖的键 != 编辑器键全集（少一个就褪色，多一个就把非法键染成合法色）');
});

// ═══════════════════════════════════════════════════════════════════
// 三、真扫描：全键标签里每个键都被正确的规则吃掉
// ═══════════════════════════════════════════════════════════════════

test('扫描: 全键 @layer 的每个键都命中预期规则（无规则被遮蔽）', () => {
    const { events, covered } = scanLayerTagBody(CANONICAL_BODY);
    assert.ok(covered, '扫描未覆盖整个内容');

    const listKeys = new Set(alternationKeys(repo['layer-list-value'].begin, 1));
    const enumKeys = new Set(alternationKeys(repo['layer-enum-value'].match, 1));

    const seenKeys = new Set<string>();
    for (const ev of events) {
        if (!ev.cap1) continue;
        seenKeys.add(ev.cap1);
        const expected = listKeys.has(ev.cap1) ? 'layer-list-value'
            : enumKeys.has(ev.cap1) ? 'layer-enum-value'
            : 'layer-scalar-key';
        assert.strictEqual(ev.ruleName, expected,
            `键 '${ev.cap1}' 被 #${ev.ruleName} 吃掉，预期 #${expected}（规则顺序错会导致静默降级）`);
    }
    // 覆盖性：每个键都必须真的被扫描到（规则写错 → 键逃过扫描 → 这里失败）
    for (const k of LAYER_TAG_KEY_NAMES) {
        assert.ok(seenKeys.has(k), `键 '${k}' 在真扫描中未被任何规则命中（着色表形同虚设）`);
    }
});

test('扫描: 列表取值的整段被吃掉（内层括号不被外层 end 抢走）', () => {
    const { events } = scanLayerTagBody(CANONICAL_BODY);
    const coord = events.find(e => e.cap1 === 'coord');
    assert.ok(coord, 'coord 未被 #layer-list-value 命中');
    assert.strictEqual(coord!.text, 'coord=(0,0)',
        '列表取值必须连同括号整段消费 —— 否则外层 #layer-tag 会在第一个 `)` 处提前闭合，'
        + '坐标后半段褪回无色');

    const track = events.find(e => e.cap1 === 'track');
    assert.strictEqual(track!.text, 'track=(expect_z, entropy)', 'track 列表同理');

    const shuttle = events.find(e => e.cap1 === 'shuttle');
    assert.strictEqual(shuttle!.text, 'shuttle=(1,0)', 'shuttle 列表同理');

    // 外层区域只应终止在最后一个 `)`
    const lastEventEnd = Math.max(...events.map(e => e.end));
    assert.strictEqual(CANONICAL_BODY.slice(lastEventEnd).trim(), '',
        '全键标签的末尾应被最后一条规则消费完，不留残余');
});

test('扫描: 枚举键的取值与键名同属一个命中（负数也认）', () => {
    const { events } = scanLayerTagBody(CANONICAL_BODY);
    const st = events.find(e => e.cap1 === 'shuttle_strategy');
    assert.ok(st, 'shuttle_strategy 未被 #layer-enum-value 命中');
    assert.strictEqual(st!.text, 'shuttle_strategy=-1',
        'shuttle_strategy=-1（默认值）必须连同负号一起着色；'
        + '只认正数会让文档规定的默认值看起来像写错了');
    assert.strictEqual(st!.ruleName, 'layer-enum-value');
});

test('扫描: 列表内的可观测量与数字各自成段', () => {
    const { events } = scanLayerTagBody(CANONICAL_BODY);
    const obs = events.filter(e => e.ruleName === 'layer-list-value' && /^[a-z_]+$/.test(e.text));
    assert.deepStrictEqual(obs.map(e => e.text), ['expect_z', 'entropy'],
        'track=(...) 里的可观测量应各成一个变量色片段');
    const nums = events.filter(e => e.ruleName === 'numbers');
    assert.ok(nums.length >= 5, `坐标/数值应被 #numbers 命中，实际 ${nums.length} 段`);
});

test('扫描: 每一段都在内容范围内，且除嵌套包含外互不重叠', () => {
    const { events } = scanLayerTagBody(CANONICAL_BODY);
    // 外层列表区域事件会**包含**其内部的片段，故只要求「不交叉」：
    // 后一段要么完全在前一段之后，要么完全落在前一段之内。
    const sorted = [...events].sort((a, b) => a.start - b.start || b.end - a.end);
    for (let i = 1; i < sorted.length; i++) {
        const prev = sorted[i - 1], cur = sorted[i];
        const disjoint = cur.start >= prev.end;
        const nested = cur.end <= prev.end;
        assert.ok(disjoint || nested,
            `片段交叉：[${prev.text}] (${prev.start}..${prev.end}) 与 [${cur.text}] (${cur.start}..${cur.end})`);
    }
    for (const e of sorted) {
        assert.ok(e.start >= 0 && e.end <= CANONICAL_BODY.length, `片段越界：${e.text}`);
    }
});
