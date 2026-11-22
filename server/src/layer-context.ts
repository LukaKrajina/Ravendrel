export function insideLayerTagKeys(before: string): boolean {
    const at = before.lastIndexOf('@layer(');
    if (at < 0) return false;
    const tail = before.slice(at + '@layer('.length);
    let depth = 0;
    for (const c of tail) {
        if (c === '(') depth++;
        else if (c === ')') {
            if (depth === 0) return false;
            depth--;
        }
    }
    if (depth !== 0) return false;
    return /(?:^|[\s,])$/.test(tail);
}












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