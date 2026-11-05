// ============================================================================
// layer-context.ts —— `@layer(...)` 内的光标上下文定位
//
// 为什么需要它：新机制把 8 个键塞进了 `@layer(...)`，于是「这个位置的合法输入
// 是什么」变得远比过去重要 —— `@layer(` 后只能写键名，`coord=(` 后只能写数字。
// 补全与悬停都需要知道光标落在哪一侧。
//
// 为什么单独成模块：这些是**纯文本位置判定**，与 LSP 连接无关。
// 留在 server.ts 里就只能靠跑起一个 LSP 会话才能测到 —— 而它们恰恰是最容易
// 在边界情形（嵌套括号、标签已闭合、键在取值位置）出错的地方。
// 见 server/src/layer-context.test.ts。
// ============================================================================

/**
 * 判断 `before`（光标前的全文）末尾是否处于 `@layer(...)` 的**键位**。
 *
 * 判据：最近一个 `@layer(` 之后没有闭合的 `)`，且内部括号深度为 0，
 * 且末尾紧邻空白 / 逗号（即位于键名起点）。
 *
 * 深度检查是必要的：`coord=(0,` 的末尾同样是「逗号结尾」，
 * 但那里只能写数字 —— 弹出键名会误导用户写出 `coord=(0, time=1)`。
 */
export function insideLayerTagKeys(before: string): boolean {
    const at = before.lastIndexOf('@layer(');
    if (at < 0) return false;
    const tail = before.slice(at + '@layer('.length);
    let depth = 0;
    for (const c of tail) {
        if (c === '(') depth++;
        else if (c === ')') {
            if (depth === 0) return false;  // 标签已闭合，光标在其后
            depth--;
        }
    }
    if (depth !== 0) return false;          // 在 coord=(...) / track=(...) 内部
    return /(?:^|[\s,])$/.test(tail);       // 必须处于键名位置
}

/**
 * 若 `offset` 位于某个 `@layer(...)` 内，返回该标签的起始下标，否则 null。
 *
 * 从前向后维护括号栈，栈中记录每个 `(` 是否为 `@layer(` 的开括号
 * （是则记其 `@layer` 起始下标，否则记 −1）。
 * 扫描到 `offset` 为止，再从**最内层向外**找第一个 `@layer`。
 *
 * 为什么不能「从 offset 向后扫、遇到第一个未匹配 `(` 就判定」：
 * `@layer(coord=(0,0))` 里光标在 `(0,0)` 中时，第一个未匹配 `(` 是
 * `coord=(`，其前缀不是 `@layer` —— 那样会得出「不在 @layer 内」的错误结论。
 * 必须继续向外找。
 */
export function enclosingLayerTag(text: string, offset: number): number | null {
    const stack: number[] = [];
    const limit = Math.min(offset, text.length - 1);
    for (let i = 0; i <= limit; i++) {
        const c = text[i];
        if (c === '(') {
            stack.push(text.slice(Math.max(0, i - 6), i) === '@layer' ? i - 6 : -1);
        } else if (c === ')') {
            stack.pop();
        }
    }
    for (let k = stack.length - 1; k >= 0; k--) {
        if (stack[k] >= 0) return stack[k];
    }
    return null;
}

/**
 * 取 `tokenStart` 之前形如 `<key>=` 的键名（用于枚举取值悬停）。
 * 返回 null 表示该数值不在键值位置上。
 */
export function layerKeyBefore(text: string, tokenStart: number): string | null {
    let j = tokenStart - 1;
    while (j >= 0 && /\s/.test(text[j])) j--;
    if (j < 0 || text[j] !== '=') return null;
    let k = j - 1;
    while (k >= 0 && /\s/.test(text[k])) k--;
    let m = k;
    while (m >= 0 && /\w/.test(text[m])) m--;
    return text.slice(m + 1, k + 1) || null;
}
