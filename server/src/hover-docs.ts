// ============================================================================
// hover-docs.ts —— rvdl 关键字 / 类型 / 内置函数的悬停详细文档
//
// 为 VSCode 悬停提示提供「作用 / 语法 / 参数 / 返回值 / 示例」的 Markdown 说明，
// 内容与 docs/rvdl-language-manual.md保持一致，作为编辑器内的轻量参考。
//
// 同时提供中文（zh）与英文（en）双语说明：根据开发者系统语言（LSP Initialize
// 的 `locale` 字段）自动选择显示语言。
// ============================================================================

export type Lang = 'zh' | 'en';

export interface HoverDoc {
    /** 一句话作用描述（中文） */
    purpose: string;
    /** 一句话作用描述（English） */
    purposeEn: string;
    /** 语法（代码片段，双语共用） */
    syntax?: string;
    /** 参数说明（中文） */
    params?: string;
    /** 参数说明（English） */
    paramsEn?: string;
    /** 返回值说明（中文） */
    returns?: string;
    /** 返回值说明（English） */
    returnsEn?: string;
    /** 补充说明（中文） */
    detail?: string;
    /** 补充说明（English） */
    detailEn?: string;
    /** 示例代码（双语共用） */
    example?: string;
}

/** 分节标题的多语言映射 */
const HEADERS: Record<Lang, { syntax: string; params: string; returns: string; example: string }> = {
    zh: { syntax: '语法', params: '参数', returns: '返回', example: '示例' },
    en: { syntax: 'Syntax', params: 'Parameters', returns: 'Returns', example: 'Example' },
};

/** 渲染一段悬停 Markdown（VSCode 支持 fenced code block 语法高亮）。 */
export function renderHover(kind: string, name: string, doc: HoverDoc, lang: Lang = 'zh'): string {
    const h = HEADERS[lang];
    const purpose = lang === 'en' ? doc.purposeEn : doc.purpose;
    const detail = lang === 'en' ? (doc.detailEn ?? doc.detail) : doc.detail;
    const params = lang === 'en' ? (doc.paramsEn ?? doc.params) : doc.params;
    const returns = lang === 'en' ? (doc.returnsEn ?? doc.returns) : doc.returns;

    const lines: string[] = [];
    lines.push(`**\`${name}\`** — ${kind}`);
    lines.push('');
    lines.push(purpose);
    if (detail) {
        lines.push('');
        lines.push(detail);
    }
    if (doc.syntax) {
        lines.push('');
        lines.push(`**${h.syntax}**`);
        lines.push('```rvdl');
        lines.push(doc.syntax);
        lines.push('```');
    }
    if (params) {
        lines.push('');
        lines.push(`**${h.params}**`);
        lines.push(params);
    }
    if (returns) {
        lines.push('');
        lines.push(`**${h.returns}**`);
        lines.push(returns);
    }
    if (doc.example) {
        lines.push('');
        lines.push(`**${h.example}**`);
        lines.push('```rvdl');
        lines.push(doc.example);
        lines.push('```');
    }
    return lines.join('\n');
}

/** 控制流关键字 */
export const KEYWORD_DOCS: Record<string, HoverDoc> = {
    if: {
        purpose: '条件分支：当条件为真时执行 `then` 块，否则执行可选的 `else` 块。',
        purposeEn: 'Conditional branch: runs the `then` block when the condition is true, otherwise the optional `else` block.',
        syntax: 'if (cond) {\n    // 条件为真\n} else {\n    // 条件为假\n}',
        detail: '`else if` 解析为「else 分支只含一个 IfStatement」的链式结构。',
        detailEn: '`else if` is parsed as an `else` branch containing a single IfStatement.',
        example: 'if (a > 0 && b < 10) {\n    return 1;\n} else {\n    return 0;\n}',
    },
    else: {
        purpose: '配合 `if` 提供条件为假时的分支；也可作为 `while` 的「零次执行」分支。',
        purposeEn: 'Provides the false branch for `if`; also the "zero-iteration" branch of `while`.',
        syntax: 'if (cond) { ... } else { ... }',
    },
    while: {
        purpose: '前置条件循环：条件为真时重复执行循环体。',
        purposeEn: 'Pre-condition loop: repeats the body while the condition holds.',
        syntax: 'while (cond) {\n    invariant i >= 0;   // 可选循环不变量\n    // 循环体\n} else {\n    // 循环体一次都未执行时进入\n}',
        detail: '支持 `invariant`（循环不变量，用于静态验证）与 `else`（零次执行分支）。',
        detailEn: 'Supports `invariant` (loop invariant, for static verification) and `else` (zero-iteration branch).',
        example: 'while (i < 100) {\n    i = i + 1;\n}',
    },
    for: {
        purpose: '标准三段式 for 循环：初始化、条件、更新。',
        purposeEn: 'Standard three-clause for loop: init, condition, update.',
        syntax: 'for (int32 i = 0; i < 10; i = i + 1) {\n    // 循环体\n}',
        example: 'for (int32 i = 0; i < 10; i = i + 1) {\n    outl(0x3F8, i);\n}',
    },
    return: {
        purpose: '从函数返回；`return;` 表示 void 函数空返回。',
        purposeEn: 'Returns from a function; `return;` is an empty void return.',
        syntax: 'return expr;    // 有返回值\nreturn;         // void 空返回',
        example: 'int32 add(int32 a, int32 b) {\n    return a + b;\n}',
    },
    break: {
        purpose: '跳出当前循环（`while` / `for` / `spin`）。',
        purposeEn: 'Exits the current loop (`while` / `for` / `spin`).',
        syntax: 'break;',
        detail: '语义分析器会检查 `break` 是否位于循环之外，否则报 `Reference Error`。',
        detailEn: 'The analyzer flags `break` outside a loop as a `Reference Error`.',
    },
    continue: {
        purpose: '跳过当前迭代，进入下一次循环判断。',
        purposeEn: 'Skips the current iteration and continues with the next loop check.',
        syntax: 'continue;',
        detail: '语义分析器会检查 `continue` 是否位于循环之外。',
        detailEn: 'The analyzer checks that `continue` is inside a loop.',
    },
    spawn: {
        purpose: '派生并发线程（Q-Digest 的「线程」单元）。',
        purposeEn: 'Spawns a concurrent thread (the "thread" unit of Q-Digest).',
        syntax: 'spawn {\n    // 并发执行体\n}',
        detail: '块体闭包继承外层作用域，编译为独立线程函数 `@rvdl_thread_N`，经 `rvdl_spawn` 以 `std::thread` 启动（真实并发）。',
        detailEn: 'The block closure inherits the enclosing scope and compiles to a thread function `@rvdl_thread_N`, launched via `rvdl_spawn` with `std::thread` (real concurrency).',
        example: 'spawn {\n    Qubit q = alloc();\n    h(q);\n    int32 r = measure(q);\n}',
    },
    entangle: {
        purpose: '声明两个量子比特纠缠（Q-Digest 纠缠闭包原语）。',
        purposeEn: 'Declares entanglement between two qubits (Q-Digest entanglement closure primitive).',
        syntax: 'entangle(q1, q2);',
        detail: '纠缠具有传递性；竞争检测据此构建纠缠闭包，判定跨线程破坏性操作。',
        detailEn: 'Entanglement is transitive; race detection builds the closure to flag cross-thread destructive operations.',
        example: 'entangle(q1, q2);\nentangle(q2, q3);   // q1 ~ q3',
    },
    spin: {
        purpose: '自旋循环（do-while 的量子化命名）：先执行循环体，再判断条件，至少执行一次。',
        purposeEn: 'Spin loop (quantum naming of do-while): runs the body first, then checks the condition, executing at least once.',
        syntax: 'spin {\n    // 循环体（至少执行一次）\n} while (cond);',
        example: 'spin {\n    i = i + 1;\n} while (i < 10);',
    },
    route: {
        purpose: '路由分支（switch 的量子化命名）：把判别值路由到若干路径之一。',
        purposeEn: 'Route branch (quantum naming of switch): routes a discriminant value to one of several paths.',
        syntax: 'route (discriminant) {\n    path V1: { ... }\n    path V2: { ... }\n    fallback: { ... }\n}',
        example: 'route (code) {\n    path 0: { return 10; }\n    path 1: { return 20; }\n    fallback: { return 0; }\n}',
    },
    fuse: {
        purpose: '融合-模式匹配表达式（match 的量子化命名，Hopf 余乘解构）。',
        purposeEn: 'Fusion pattern-matching expression (quantum naming of match, Hopf comultiplication).',
        syntax: 'fuse (discriminant) {\n    pattern1: value1,\n    pattern2: value2,\n    _: default\n}',
        detail: '`_` 为通配模式。',
        detailEn: '`_` is the wildcard pattern.',
        example: 'int32 label = fuse (m) {\n    0: 10,\n    1: 20,\n    _: 0\n};',
    },
    fallback: {
        purpose: '`route` 中的兜底分支，等价于 `switch` 的 `default`。',
        purposeEn: 'The fallback branch of `route`, equivalent to `switch`\'s `default`.',
        syntax: 'route (d) {\n    path V: { ... }\n    fallback: { ... }\n}',
    },
    new: {
        purpose: '栈分配构造对象。',
        purposeEn: 'Stack-allocates and constructs an object.',
        syntax: 'auto obj = new TypeName(args);',
        detail: '`new` 栈分配、`make` 堆分配（经 GC 堆）。',
        detailEn: '`new` allocates on the stack; `make` allocates on the heap (via GC).',
        example: 'auto bell = new BellState();\nint32 m = bell.measure();',
    },
    make: {
        purpose: '堆分配构造对象（经 GC 堆）。',
        purposeEn: 'Heap-allocates and constructs an object (via the GC heap).',
        syntax: 'auto obj = make TypeName(args);',
        detail: '`make` 堆分配、`new` 栈分配。',
        detailEn: '`make` allocates on the heap; `new` on the stack.',
        example: 'auto b = make BellState();',
    },
    auto: {
        purpose: '类型推断声明（与 `let` 等价）：由初始值推导变量类型。',
        purposeEn: 'Type-inference declaration (equivalent to `let`): the type is derived from the initializer.',
        syntax: 'auto x = expr;',
        example: 'auto q = alloc();      // Qubit\nauto n = 42;          // int32\nauto s = "ravendrel";     // string',
    },
    let: {
        purpose: '类型推断声明（与 `auto` 等价）：由初始值推导变量类型。',
        purposeEn: 'Type-inference declaration (equivalent to `auto`): the type is derived from the initializer.',
        syntax: 'let x = expr;',
        example: 'let f = 3.14;    // double',
    },
    fn: {
        purpose: '匿名函数（lambda / 闭包）与函数指针类型。',
        purposeEn: 'Anonymous function (lambda / closure) and function-pointer type.',
        syntax: 'let f = fn(int32 x) -> int32 {\n    return x * 2;\n};',
        detail: '函数指针类型写作 `fn<ret(params)>`（等价于 `(params)->ret`），取函数地址用 `&`。',
        detailEn: 'The function-pointer type is written `fn<ret(params)>` (equivalent to `(params)->ret`); take a function\'s address with `&`.',
        example: 'fn<int32(int32)> cb = &handler;\nint32 r = cb(41);',
    },
    result: {
        purpose: '在契约 `ensures` 中引用函数返回值。',
        purposeEn: 'References the function\'s return value inside an `ensures` contract.',
        syntax: 'ensures result >= 0;',
        example: 'int32 square(int32 x)\n    ensures result >= 0;\n{ return x * x; }',
    },
    unsafe: {
        purpose: '系统级危险操作块（裸指针解引用 / MMIO / 内联汇编）。',
        purposeEn: 'System-level unsafe block (raw pointer dereference / MMIO / inline assembly).',
        syntax: 'unsafe {\n    // 危险操作\n}',
        detail: '是「写内核」的显式危险边界；能力校验在语义层完成。',
        detailEn: 'An explicit danger boundary for "kernel writing"; capability checks happen in the semantic layer.',
        example: 'unsafe {\n    cap<int32> p = null;\n    *p = 5;\n}',
    },

    // ── 声明 / 模块 / 类型系统关键字 ──
    mod: {
        purpose: '定义模块。',
        purposeEn: 'Defines a module.',
        syntax: 'mod name {\n    // 成员\n}',
        example: 'mod math {\n    pub int32 add(int32 a, int32 b) { return a + b; }\n}',
    },
    use: {
        purpose: '路径导入。',
        purposeEn: 'Path import.',
        syntax: 'use path::to::item;',
    },
    pub: {
        purpose: '声明公开成员。',
        purposeEn: 'Declares a public member.',
        syntax: 'pub int32 f() { ... }',
    },
    form: {
        purpose: '定义数据结构（支持单继承 `inherits` 与 `rank` 秩）。',
        purposeEn: 'Defines a data structure (with single inheritance `inherits` and `rank` blocks).',
        syntax: 'form Name {\n    Type field;\n    // rank 秩块\n}',
        detail: '`@[packed] form` 定义精确字节布局结构（无 vtable、无填充）；字段支持位域 `T f : N`。',
        detailEn: '`@[packed] form` defines an exact byte layout (no vtable, no padding); fields support bitfields `T f : N`.',
        example: 'form Point {\n    double x;\n    double y;\n}',
    },
    impl: {
        purpose: '为类型实现 trait（`impl <trait> for <type>`）。',
        purposeEn: 'Implements a trait for a type (`impl <trait> for <type>`).',
        syntax: 'impl Trait for Type {\n    // 方法\n}',
        example: 'impl Shape for Circle {\n    double area() { return 3.14; }\n}',
    },
    trait: {
        purpose: '定义可共享的行为接口。',
        purposeEn: 'Defines a shareable behavior interface.',
        syntax: 'trait Name {\n    Ret method(Params);\n}',
        example: 'trait Shape {\n    double area();\n}',
    },
    template: {
        purpose: '泛型声明（作用于 `form` / `impl`）。',
        purposeEn: 'Generic declaration (applies to `form` / `impl`).',
        syntax: 'template<T> form Box {\n    T value;\n}',
        example: 'template<T> form Box { T value; }\nauto b = new Box<int32>();',
    },
    rank: {
        purpose: '在 `form` / `trait` 中声明秩块（将字段与方法分组成不同秩）。',
        purposeEn: 'Declares a rank block in `form` / `trait` (groups fields and methods into ranks).',
        syntax: 'form Name {\n    rank classical { ... }\n    rank quantum { ... }\n}',
    },
    self: {
        purpose: '方法接收者（`self` 按值 / `&self` 借用）。',
        purposeEn: 'Method receiver (`self` by value / `&self` by borrow).',
        syntax: 'Ret method(self, ...) { ... }\nRet method(&self, ...) { ... }',
    },
    lattice: {
        purpose: '晶格数组类型（`lattice<T, B>`），支持多维下标访问 `a[x, y]`。',
        purposeEn: 'Lattice array type (`lattice<T, B>`), supports multi-dimensional indexing `a[x, y]`.',
        syntax: 'lattice<int32, open> board = ...;\nint32 v = board[x, y];',
    },
    cap: {
        purpose: '能力指针类型（CHERI 式：地址 + 边界 + 权限 + 有效性）。',
        purposeEn: 'Capability pointer type (CHERI-style: address + bounds + permission + validity).',
        syntax: 'cap<int32> p = null;\n*p = 5;\nint32 x = *p;',
        detail: '在 LLVM 层降为普通指针 `T*`，语义层受借用检查与 `unsafe` 约束。',
        detailEn: 'Lowers to a plain pointer `T*` in LLVM, constrained by the borrow checker and `unsafe`.',
    },
    native: {
        purpose: '内联汇编（逃逸到目标机器原生指令集）。',
        purposeEn: 'Inline assembly (escapes to the target machine\'s native instruction set).',
        syntax: 'native("hlt");\nnative("lidt (${0})", &idtr);',
        detail: '模板用 `${0}`/`${1}` 占位引用操作数，操作数默认 `"r"` 约束。须在 `unsafe` 块内。',
        detailEn: 'Templates reference operands with `${0}`/`${1}`; operands default to the `"r"` constraint. Must be inside an `unsafe` block.',
    },
    fixed: {
        purpose: '编译期常量（`const` 的量子化命名，确定型范式）。',
        purposeEn: 'Compile-time constant (quantum naming of `const`, the definite paradigm).',
        syntax: 'fixed int32 MAX = 100;',
        example: 'fixed int32 MAX = 100;\nint32 x = MAX;',
    },
    flavor: {
        purpose: '味（`enum` 的量子化命名）：具名常量，按声明顺序自动赋值。',
        purposeEn: 'Flavor (quantum naming of `enum`): named constants auto-assigned in declaration order.',
        syntax: 'flavor Name {\n    A,\n    B = 5,\n    C       // C 自动 = 6\n}',
        example: 'flavor Channel { DEPOLARIZING, DEPHASING, AMPLITUDE_DAMPING, PAULI }\nint32 m = Channel.PAULI;   // 3',
    },
    morph: {
        purpose: '态射宏：全局、卫生的编译期宏系统，在编译期把「语法模式」变换为「代码」。',
        purposeEn: 'Morph: the global, hygienic compile-time macro system — transforms a syntax pattern into code.',
        syntax: 'morph 名字($元变量: 种类, ...) {\n    模板\n}\n\n// 调用（统一 name!(...) 形式）\n名字!(实参, ...);',
        params: '元变量种类：`expr`（表达式）、`stmt`（语句）、`items`（0+ 顶层项）、`type`（类型，含泛型后缀）、`ident`（标识符）、`literal`（字面量）、`tt`（任意 token 树）。重复用 `$(...)*` / `$(...)+`；`#$名字` 把捕获片段字符串化。',
        paramsEn: 'Metavariable kinds: `expr` (expression), `stmt` (statement), `items` (0+ top-level items), `type` (type, incl. generic suffix), `ident` (identifier), `literal` (number/string), `tt` (any token tree). Repetition uses `$(...)*` / `$(...)+`; `#$name` stringifies the captured fragment.',
        detail: '三条硬约束：① **全局** —— 仅顶层声明，与内建同命名空间，不允许局部宏，展开不依赖局部作用域；② **卫生 = 规范不变性** —— 模板引入的绑定（`let` / `fn` 名 / 参数 / 类型声明）在每次展开时 α-重命名取新原子，杜绝变量捕获；③ **不使用 #define** —— 展开发生在词法层（parse 之前），产物重新进入类型 / 借用 / 拓扑检查。宏可展开出宏，限深递归（上限 64）。\n\n**约定**：模板是不含末尾分号的语句/表达式序列，调用点照常写 `;`；需要保证优先级时请在模板中自行加括号 `($e)`。',
        detailEn: 'Three invariants: (1) **global** — top-level only, sharing the namespace with builtins, no local macros, expansion independent of local scope; (2) **hygiene as gauge invariance** — binders introduced by the template (`let`, `fn` names, parameters, typed declarations) are alpha-renamed to fresh atoms on every expansion, preventing variable capture; (3) **no #define** — expansion happens at the lexical layer (before parsing) and the result re-enters type / borrow / topology checking. A macro may expand into another macro, with a depth limit of 64.\n\n**Convention**: a template is a statement/expression sequence *without* the trailing semicolon — supply `;` at the call site. Parenthesize explicitly `($e)` when precedence matters.',
        example: 'morph twice($e: expr) { ($e) + ($e) }\nint32 y = twice!(x * 3);      // → (x * 3) + (x * 3)\n\nmorph make_syscalls($($name: ident),*) {\n    $(fn $name() -> int32 { return 0; })*\n}\nmake_syscalls!(do_init, do_tick);   // 生成 fn do_init() 与 fn do_tick()',
    },
    addr: {
        purpose: '取函数地址（返回低 32 位整数地址）。',
        purposeEn: 'Takes a function\'s address (returns the low 32 bits).',
        syntax: 'int32 h = addr(&fn);',
    },
    basis_state: {
        purpose: '在任意基（布洛赫方向 θ, φ）下构建量子态。',
        purposeEn: 'Builds a quantum state in an arbitrary basis (Bloch direction θ, φ).',
        syntax: 'auto q = basis_state(theta, phi, value);',
        params: '`theta`（double）极角、`phi`（double）方位角、`value`（int32）0 为 + 本征态 / 1 为 - 本征态。',
        paramsEn: '`theta` (double) polar angle, `phi` (double) azimuth, `value` (int32) 0 for the + eigenstate / 1 for the - eigenstate.',
        returns: '`QObject`',
        returnsEn: '`QObject`',
        example: 'auto xplus = basis_state(1.5707963, 0.0, 0);   // X 基 |+⟩',
    },
    export: {
        purpose: '导出符号（模块成员或函数）。',
        purposeEn: 'Exports a symbol (module member or function).',
        syntax: 'export int32 f() { ... }',
    },
    import: {
        purpose: '导入 `.rvdli` 模块（配合 `from`）。',
        purposeEn: 'Imports a `.rvdli` module (with `from`).',
        syntax: 'import alias from "./module.rvdli";',
    },
    extern: {
        purpose: '外部 C 符号声明（FFI），调用原生动态库函数。',
        purposeEn: 'External C symbol declaration (FFI), calls a native dynamic-library function.',
        syntax: 'extern <ret> <name>(<params>);',
    },
    requires: {
        purpose: '声明权限（模块权限）或函数前置条件。',
        purposeEn: 'Declares a permission (module permission) or a function precondition.',
        syntax: 'requires io.network;                    // 权限\nfn f() requires x >= 0; { ... }   // 前置条件',
    },
    ensures: {
        purpose: '函数后置条件（静态验证契约）。',
        purposeEn: 'Function postcondition (static verification contract).',
        syntax: 'fn f() ensures result >= 0; { ... }',
    },
    invariant: {
        purpose: '循环不变量（用于 `while` / `for` 静态验证）。',
        purposeEn: 'Loop invariant (for `while` / `for` static verification).',
        syntax: 'while (cond) {\n    invariant i >= 0;\n    ...\n}',
    },
    from: {
        purpose: '配合 `import` 指定模块来源路径。',
        purposeEn: 'Specifies the module source path together with `import`.',
        syntax: 'import alias from "./module.rvdli";',
    },
    path: {
        purpose: '`route` 中的取值分支标签。',
        purposeEn: 'A value-branch label inside `route`.',
        syntax: 'route (d) {\n    path V: { ... }\n}',
    },
    asm: {
        purpose: '裸汇编块（多指令 / 多寄存器序列）。',
        purposeEn: 'Raw assembly block (multi-instruction / multi-register sequence).',
        syntax: 'asm {\n    "push rax; push rbx;"\n    "iretq;"\n}',
        detail: '块内字符串按行拼接，不做 `${}` 占位符替换；与 `@[naked]` 裸函数配合时直接发射为裸汇编体。',
        detailEn: 'Strings inside are joined by lines without `${}` substitution; with `@[naked]` it emits the raw body directly.',
    },
};

/** 类型关键字 */
export const TYPE_DOCS: Record<string, HoverDoc> = {
    int: { purpose: '32 位有符号整数（`int32` 的别名）。', purposeEn: '32-bit signed integer (alias of `int32`).', detail: 'LLVM 映射：`i32`。', detailEn: 'LLVM mapping: `i32`.', example: 'int x = 42;' },
    int8: { purpose: '8 位有符号整数。', purposeEn: '8-bit signed integer.', detail: 'LLVM 映射：`i8`。', detailEn: 'LLVM mapping: `i8`.' },
    int16: { purpose: '16 位有符号整数。', purposeEn: '16-bit signed integer.', detail: 'LLVM 映射：`i16`。', detailEn: 'LLVM mapping: `i16`.' },
    int32: { purpose: '32 位有符号整数。', purposeEn: '32-bit signed integer.', detail: 'LLVM 映射：`i32`。', detailEn: 'LLVM mapping: `i32`.', example: 'int32 x = 42;' },
    int64: { purpose: '64 位有符号整数。', purposeEn: '64-bit signed integer.', detail: 'LLVM 映射：`i64`。', detailEn: 'LLVM mapping: `i64`.' },
    uint8: { purpose: '8 位无符号整数。', purposeEn: '8-bit unsigned integer.', detail: 'LLVM 映射：`i8`（无符号语义）。', detailEn: 'LLVM mapping: `i8` (unsigned semantics).' },
    uint16: { purpose: '16 位无符号整数。', purposeEn: '16-bit unsigned integer.', detail: 'LLVM 映射：`i16`。', detailEn: 'LLVM mapping: `i16`.' },
    uint32: { purpose: '32 位无符号整数。', purposeEn: '32-bit unsigned integer.', detail: 'LLVM 映射：`i32`。', detailEn: 'LLVM mapping: `i32`.' },
    uint64: { purpose: '64 位无符号整数。', purposeEn: '64-bit unsigned integer.', detail: 'LLVM 映射：`i64`，走 `udiv`/`urem`/`lshr` 无符号语义，承载 48 位 LBA / GPT 64 位字段。', detailEn: 'LLVM mapping: `i64`, uses `udiv`/`urem`/`lshr` unsigned semantics, holds 48-bit LBA / GPT 64-bit fields.' },
    float: { purpose: '单精度浮点。', purposeEn: 'Single-precision float.', detail: 'LLVM 映射：`float`。', detailEn: 'LLVM mapping: `float`.' },
    double: { purpose: '双精度浮点。', purposeEn: 'Double-precision float.', detail: 'LLVM 映射：`double`。', detailEn: 'LLVM mapping: `double`.' },
    complex64: { purpose: '单精度复数（量子态矢量运算）。', purposeEn: 'Single-precision complex (quantum state-vector arithmetic).', detail: 'LLVM 映射：`{ float, float }`。', detailEn: 'LLVM mapping: `{ float, float }`.', example: 'complex64 z = complex(1.0, 2.0);' },
    complex128: { purpose: '双精度复数（量子态矢量运算）。', purposeEn: 'Double-precision complex (quantum state-vector arithmetic).', detail: 'LLVM 映射：`{ double, double }`。', detailEn: 'LLVM mapping: `{ double, double }`.', example: 'complex128 z = complex(1.0, 2.0);' },
    arr: { purpose: '定长数组类型 `arr<T, N>`（静态查找表）。', purposeEn: 'Fixed-size array type `arr<T, N>` (static lookup table).', detail: 'LLVM 映射：`[N x T]`。', detailEn: 'LLVM mapping: `[N x T]`.', syntax: 'arr<int32, 4> a = [1, 2, 3, 4];', example: 'fixed arr<int32, 4> SCANCODES = [0x1E, 0x30, 0x2E, 0x20];' },
    string: { purpose: '字符串。', purposeEn: 'String.', detail: 'LLVM 映射：`i8*`。', detailEn: 'LLVM mapping: `i8*`.', example: 'string s = "ravendrel";' },
    char: { purpose: '字符。', purposeEn: 'Character.', detail: 'LLVM 映射：`i8`。', detailEn: 'LLVM mapping: `i8`.' },
    bool: { purpose: '布尔类型。', purposeEn: 'Boolean type.', detail: 'LLVM 映射：`i1`。比较运算返回 `bool`。', detailEn: 'LLVM mapping: `i1`; comparisons return `bool`.' },
    void: { purpose: '空类型（无返回值）。', purposeEn: 'Void type (no return value).', detail: 'LLVM 映射：`void`。', detailEn: 'LLVM mapping: `void`.', example: 'void do_nothing() { return; }' },
    Qubit: { purpose: '单个量子比特，不可克隆。', purposeEn: 'A single qubit, non-clonable.', detail: 'LLVM 映射：`%Qubit*`（opaque）。受量子线性类型（QLT）no-cloning 约束。', detailEn: 'LLVM mapping: `%Qubit*` (opaque). Subject to QLT no-cloning.', example: 'Qubit q = alloc();' },
    QObject: { purpose: '量子对象（可容纳多 qubit 状态）。', purposeEn: 'Quantum object (may hold multi-qubit state).', detail: 'LLVM 映射：`%QObject*`（opaque）。', detailEn: 'LLVM mapping: `%QObject*` (opaque).' },
    QModel: { purpose: '量子语言模型。', purposeEn: 'Quantum language model.', detail: 'LLVM 映射：`%QModel*`（opaque）。支持 `.export(path)` 导出 `.rvdlm`。', detailEn: 'LLVM mapping: `%QModel*` (opaque). Supports `.export(path)` to `.rvdlm`.' },
    QRegister: { purpose: '量子寄存器类型。', purposeEn: 'Quantum register type.', detail: '用于量子寄存器操作。', detailEn: 'Used for quantum-register operations.' },
    Result: { purpose: '测量结果类型。', purposeEn: 'Measurement result type.', detail: '量子测量的结果。', detailEn: 'The result of a quantum measurement.' },
    DiracState: { purpose: '狄拉克态（n 维）。', purposeEn: 'Dirac state (n-dimensional).', syntax: 'auto s = new DiracState(n);', returns: '`QObject`', returnsEn: '`QObject`' },
    BellState: { purpose: 'Bell 态。', purposeEn: 'Bell state.', syntax: 'auto b = new BellState();', returns: '`QObject`', returnsEn: '`QObject`' },
    QuantumRegister: { purpose: 'n 比特量子寄存器。', purposeEn: 'n-qubit quantum register.', syntax: 'auto reg = new QuantumRegister(n);', returns: '`QObject`', returnsEn: '`QObject`' },
    QReservoir: { purpose: '量子存储池（QReservoir）。', purposeEn: 'Quantum reservoir.', detail: '用于量子资源管理。', detailEn: 'Used for quantum resource management.' },
};

/** 内置函数 */
export const FUNCTION_DOCS: Record<string, HoverDoc> = {
    // ── 量子核心 ──
    alloc: { purpose: '分配一个量子比特。', purposeEn: 'Allocates a qubit.', syntax: 'alloc()', returns: '`Qubit`', returnsEn: '`Qubit`', example: 'Qubit q = alloc();' },
    measure: { purpose: '测量量子比特并坍缩，返回 0/1。', purposeEn: 'Measures a qubit and collapses it, returning 0/1.', syntax: 'measure(q)', params: '`q`（Qubit）待测量比特。', paramsEn: '`q` (Qubit) the qubit to measure.', returns: '`int32`', returnsEn: '`int32`', example: 'int32 r = measure(q);' },
    measure_x: { purpose: 'X 基测量量子比特。', purposeEn: 'Measures a qubit in the X basis.', syntax: 'measure_x(q)', params: '`q`（Qubit）。', paramsEn: '`q` (Qubit).', returns: '`int32`', returnsEn: '`int32`' },
    measure_y: { purpose: 'Y 基测量量子比特。', purposeEn: 'Measures a qubit in the Y basis.', syntax: 'measure_y(q)', params: '`q`（Qubit）。', paramsEn: '`q` (Qubit).', returns: '`int32`', returnsEn: '`int32`' },
    basis_state: { purpose: '在任意基（θ, φ）下构建量子态。', purposeEn: 'Builds a quantum state in an arbitrary basis (θ, φ).', syntax: 'basis_state(theta, phi, value)', params: '`theta`/`phi`（double）布洛赫方向，`value`（int32）0/1。', paramsEn: '`theta`/`phi` (double) Bloch direction, `value` (int32) 0/1.', returns: '`QObject`', returnsEn: '`QObject`' },

    // ── 量子门 ──
    h: { purpose: 'Hadamard 门。', purposeEn: 'Hadamard gate.', syntax: 'h(q)', params: '`q`（Qubit）。', paramsEn: '`q` (Qubit).' },
    x: { purpose: 'Pauli-X 门（NOT）。', purposeEn: 'Pauli-X gate (NOT).', syntax: 'x(q)', params: '`q`（Qubit）。', paramsEn: '`q` (Qubit).' },
    y: { purpose: 'Pauli-Y 门。', purposeEn: 'Pauli-Y gate.', syntax: 'y(q)', params: '`q`（Qubit）。', paramsEn: '`q` (Qubit).' },
    z: { purpose: 'Pauli-Z 门（相位翻转）。', purposeEn: 'Pauli-Z gate (phase flip).', syntax: 'z(q)', params: '`q`（Qubit）。', paramsEn: '`q` (Qubit).' },
    s: { purpose: 'S 门（π/2 相位门，= Rz(π/2)）。', purposeEn: 'S gate (π/2 phase gate, = Rz(π/2)).', syntax: 's(q)', params: '`q`（Qubit）。', paramsEn: '`q` (Qubit).' },
    t: { purpose: 'T 门（π/4 相位门，= Rz(π/4)）。', purposeEn: 'T gate (π/4 phase gate, = Rz(π/4)).', syntax: 't(q)', params: '`q`（Qubit）。', paramsEn: '`q` (Qubit).' },
    rz: { purpose: '绕 Z 轴旋转门。', purposeEn: 'Rotation gate about the Z axis.', syntax: 'rz(q, angle)', params: '`q`（Qubit）、`angle`（double）旋转角。', paramsEn: '`q` (Qubit), `angle` (double).' },
    rx: { purpose: '绕 X 轴旋转门。', purposeEn: 'Rotation gate about the X axis.', syntax: 'rx(q, angle)', params: '`q`（Qubit）、`angle`（double）旋转角。', paramsEn: '`q` (Qubit), `angle` (double).' },
    ry: { purpose: '绕 Y 轴旋转门。', purposeEn: 'Rotation gate about the Y axis.', syntax: 'ry(q, angle)', params: '`q`（Qubit）、`angle`（double）旋转角。', paramsEn: '`q` (Qubit), `angle` (double).' },
    cnot: { purpose: '受控非门（控制, 目标）。', purposeEn: 'Controlled-NOT gate (control, target).', syntax: 'cnot(control, target)', params: '`control`、`target`（Qubit）。', paramsEn: '`control`, `target` (Qubit).' },
    toffoli: { purpose: 'Toffoli 门（两控制一目标）。', purposeEn: 'Toffoli gate (two controls, one target).', syntax: 'toffoli(c1, c2, target)', params: '两个控制位 + 一个目标位（Qubit）。', paramsEn: 'Two controls + one target (Qubit).' },
    swap: { purpose: '交换两个量子比特。', purposeEn: 'Swaps two qubits.', syntax: 'swap(q1, q2)', params: '`q1`、`q2`（Qubit）。', paramsEn: '`q1`, `q2` (Qubit).' },
    qft: { purpose: '量子傅里叶变换。', purposeEn: 'Quantum Fourier transform.', syntax: 'qft(n)', params: '`n`（int）比特数。', paramsEn: '`n` (int) number of qubits.' },
    iqft: { purpose: '逆量子傅里叶变换（qft 的可逆对偶）。', purposeEn: 'Inverse quantum Fourier transform (reversible dual of qft).', syntax: 'iqft(n)', params: '`n`（int）比特数。', paramsEn: '`n` (int) number of qubits.' },
    braid: { purpose: '编织门（Yang-Baxter）。', purposeEn: 'Braid gate (Yang-Baxter).', syntax: 'braid(q1, q2)', params: '`q1`、`q2`（Qubit）。', paramsEn: '`q1`, `q2` (Qubit).' },
    cx: { purpose: '受控 X（= CNOT）。', purposeEn: 'Controlled X (= CNOT).', syntax: 'cx(control, target)' },
    ch: { purpose: '受控 Hadamard。', purposeEn: 'Controlled Hadamard.', syntax: 'ch(control, target)' },
    crz: { purpose: '受控 Rz。', purposeEn: 'Controlled Rz.', syntax: 'crz(control, target, angle)' },
    cswap: { purpose: '受控 SWAP（Fredkin）。', purposeEn: 'Controlled SWAP (Fredkin).', syntax: 'cswap(c, q1, q2)' },
    c_toffoli: { purpose: '受控 Toffoli（C³X）。', purposeEn: 'Controlled Toffoli (C³X).', syntax: 'c_toffoli(c, c1, c2, target)' },
    cqft: { purpose: '受控 QFT。', purposeEn: 'Controlled QFT.', syntax: 'cqft(control, n)' },
    cbraid: { purpose: '受控编织（Yang-Baxter）。', purposeEn: 'Controlled braid (Yang-Baxter).', syntax: 'cbraid(c, q1, q2)' },

    // ── 文本 / 图像编码 ──
    encode_text: { purpose: '将文本编码为量子对象。', purposeEn: 'Encodes text into a quantum object.', syntax: 'encode_text(s)', params: '`s`（string）。', paramsEn: '`s` (string).', returns: '`QObject`', returnsEn: '`QObject`' },
    encode_image: { purpose: '将图像加载并振幅编码为量子对象。', purposeEn: 'Loads an image and amplitude-encodes it into a quantum object.', syntax: 'encode_image(path, num_qubits)', params: '`path`（string）图像路径（PNG/JPEG/BMP/PGM 等）；`num_qubits`（int32）编码 qubit 数（像素下采样到 2^num_qubits）。', paramsEn: '`path` (string) image path; `num_qubits` (int32) encoding qubit count.', returns: '`QObject`', returnsEn: '`QObject`' },
    rvdl_encode_string: { purpose: '将字符串编码为量子对象。', purposeEn: 'Encodes a string into a quantum object.', syntax: 'rvdl_encode_string(s)', returns: '`QObject`', returnsEn: '`QObject`' },
    rvdl_decode_string: { purpose: '将量子对象解码为字符串。', purposeEn: 'Decodes a quantum object into a string.', syntax: 'rvdl_decode_string(q)', returns: '`string`', returnsEn: '`string`' },

    // ── 量子语言模型（QLM）──
    qlm_invoke: { purpose: '调用量子语言模型推理。', purposeEn: 'Invokes quantum language model inference.', syntax: 'qlm_invoke(data, steps, lr)', params: '`data`（QObject）、`steps`（int）、`lr`（double）。', paramsEn: '`data` (QObject), `steps` (int), `lr` (double).', returns: '`QModel`', returnsEn: '`QModel`' },
    qlm_load: { purpose: '加载量子语言模型。', purposeEn: 'Loads a quantum language model.', syntax: 'qlm_load(path)', params: '`path`（string）。', paramsEn: '`path` (string).', returns: '`QModel`', returnsEn: '`QModel`' },
    qlm_forward: { purpose: '量子语言模型前向传播。', purposeEn: 'Quantum language model forward pass.', syntax: 'qlm_forward(model, input)', params: '`model`（QModel）、`input`（QObject）。', paramsEn: '`model` (QModel), `input` (QObject).', returns: '`void`', returnsEn: '`void`' },

    // ── 脑机接口（QbNS）──
    mind_read: { purpose: '读取脑机接口信号。', purposeEn: 'Reads brain-computer interface signals.', syntax: 'mind_read(modality)', params: '`modality`（string）：`stream/spike/lfp/eeg/sensor`。', paramsEn: '`modality` (string): `stream/spike/lfp/eeg/sensor`.', returns: '`QObject`', returnsEn: '`QObject`' },
    mind_train: { purpose: '训练脑机接口。', purposeEn: 'Trains the brain-computer interface.', syntax: 'mind_train(data, steps, lr)', returns: '`void`', returnsEn: '`void`' },
    mind_feedback: { purpose: '脑机接口反馈。', purposeEn: 'Brain-computer interface feedback.', syntax: 'mind_feedback(data)', returns: '`void`', returnsEn: '`void`' },
    veda_qlm_train: { purpose: 'VedaROS 量子语言模型训练。', purposeEn: 'VedaROS quantum language model training.', syntax: 'veda_qlm_train(data, steps, lr)', returns: '`void`', returnsEn: '`void`' },

    // ── 标量数学 / 神经原语 ──
    surrogate: { purpose: '代理梯度函数。', purposeEn: 'Surrogate gradient function.', syntax: 'surrogate(x, a, b)', params: '`x`、`a`、`b`（double）。', paramsEn: '`x`, `a`, `b` (double).', returns: '`double`', returnsEn: '`double`' },
    tanh_quantize: { purpose: 'tanh 量化。', purposeEn: 'tanh quantization.', syntax: 'tanh_quantize(x, scale, bits)', returns: '`double`', returnsEn: '`double`' },
    lif_step: { purpose: 'LIF 神经元单步演化。', purposeEn: 'LIF neuron single-step evolution.', syntax: 'lif_step(v, i, tau, dt)', returns: '`double`', returnsEn: '`double`' },
    mellowmax2: { purpose: 'Mellowmax 算子。', purposeEn: 'Mellowmax operator.', syntax: 'mellowmax2(x, y, alpha)', returns: '`double`', returnsEn: '`double`' },
    logsumexp2: { purpose: 'LogSumExp 算子。', purposeEn: 'LogSumExp operator.', syntax: 'logsumexp2(x, y, alpha)', returns: '`double`', returnsEn: '`double`' },
    boltzmann2: { purpose: 'Boltzmann 算子。', purposeEn: 'Boltzmann operator.', syntax: 'boltzmann2(x, y, alpha)', returns: '`double`', returnsEn: '`double`' },
    tnorm_luk: { purpose: 'Łukasiewicz t-范数。', purposeEn: 'Łukasiewicz t-norm.', syntax: 'tnorm_luk(a, b)', returns: '`double`', returnsEn: '`double`' },
    tnorm_prod: { purpose: '乘积 t-范数。', purposeEn: 'Product t-norm.', syntax: 'tnorm_prod(a, b)', returns: '`double`', returnsEn: '`double`' },
    tnorm_godel: { purpose: 'Gödel t-范数。', purposeEn: 'Gödel t-norm.', syntax: 'tnorm_godel(a, b)', returns: '`double`', returnsEn: '`double`' },
    polymer_weight: { purpose: '聚合物权重。', purposeEn: 'Polymer weight.', syntax: 'polymer_weight(x, y, z)', returns: '`double`', returnsEn: '`double`' },
    polymer_mix_bound: { purpose: '聚合物混合界。', purposeEn: 'Polymer mixing bound.', syntax: 'polymer_mix_bound(a, b)', returns: '`double`', returnsEn: '`double`' },
    // ── nqcmath / expermath（QMathAbi.hpp）──
    qstate_purity: { purpose: '量子态纯度 Tr(ρ²)。', purposeEn: 'Quantum state purity Tr(ρ²).', syntax: 'qstate_purity(q)', params: '`q`（QObject）。', paramsEn: '`q` (QObject).', returns: '`double`（纯态 1，最大混合态 1/d）', returnsEn: '`double` (1 for pure, 1/d for maximally mixed)' },
    qstate_linear_entropy: { purpose: '线性熵 1 − Tr(ρ²)。', purposeEn: 'Linear entropy 1 − Tr(ρ²).', syntax: 'qstate_linear_entropy(q)', params: '`q`（QObject）。', paramsEn: '`q` (QObject).', returns: '`double`', returnsEn: '`double`' },
    qstate_renyi_entropy: { purpose: 'Rényi 熵 S_α(ρ)。', purposeEn: 'Rényi entropy S_α(ρ).', syntax: 'qstate_renyi_entropy(q, alpha)', params: '`q`（QObject）、`alpha`（double，α=1 退化为 von Neumann 熵）。', paramsEn: '`q` (QObject), `alpha` (double, α=1 reduces to von Neumann).', returns: '`double`', returnsEn: '`double`' },
    math_dot: { purpose: '向量点积 Σ a[i]·b[i]（并行归约）。', purposeEn: 'Vector dot product Σ a[i]·b[i] (parallel reduction).', syntax: 'math_dot(a, b, n)', params: '`a`、`b`（arr<double,N>）、`n`（int32）。', paramsEn: '`a`, `b` (arr<double,N>), `n` (int32).', returns: '`double`', returnsEn: '`double`' },
    tau_alpha: { purpose: '时间缩微年度因子 α：preset 0 移动 1.3 / 1 自动驾驶 1.5 / 2 AI 10.0。', purposeEn: 'Annual τ-scaling factor α: preset 0 mobile 1.3 / 1 autonomous 1.5 / 2 AI 10.0.', syntax: 'tau_alpha(preset)', params: '`preset`（double：领域下标）。', paramsEn: '`preset` (double: domain index).', returns: '`double`（1.3 / 1.5 / 10.0，未知 1.0）', returnsEn: '`double` (1.3 / 1.5 / 10.0, unknown 1.0)' },
    tau_generational: { purpose: '代际时间缩微 τ/α（韬定律：世代间将以年度缩放因子压缩时间常数）。', purposeEn: 'Generational time scaling τ/α (τ scaling law).', syntax: 'tau_generational(tau, alpha)', params: '`tau`（本代时间预算）、`alpha`（缩放因子）。', paramsEn: '`tau` (current time budget), `alpha` (scaling factor).', returns: '`double`（τ/α，α ≤ 0 返原值）', returnsEn: '`double` (τ/α; returns τ if α ≤ 0)' },
    tau_fold_depth: { purpose: '折叠深度：串行深度 L 折进 T 个并行层后的关键路径 ⌈L/T⌉（LogicFolding 的量子化）。', purposeEn: 'Folded critical-path depth ⌈L/T⌉ after folding serial depth L into T parallel tiers.', syntax: 'tau_fold_depth(L, T)', params: '`L`（串行深度）、`T`（tier 数）。', paramsEn: '`L` (serial depth), `T` (number of tiers).', returns: '`double`（折叠后深度）', returnsEn: '`double` (folded depth)' },
    tau_gear_ratio: { purpose: '齿轮比 g = 跨层边/层内边（论文阈值为 3：大于 3 时折叠不划算）。', purposeEn: 'Gear ratio g = cross-tier / intra-tier edges (paper threshold 3).', syntax: 'tau_gear_ratio(cross, intra)', params: '`cross`（跨层边数）、`intra`（层内边数）。', paramsEn: '`cross` (cross-tier edges), `intra` (intra-tier edges).', returns: '`double`（g；intra=0 且 cross>0 时取大数）', returnsEn: '`double` (g; large number if intra=0 and cross>0)' },
    tau_route_cost: { purpose: '调度总代价 τ_gate·D + τ_comm·D·g（关键路径×单步开销 + 跨层通信×齿轮比）。', purposeEn: 'Schedule cost τ_gate·D + τ_comm·D·g (critical path plus cross-tier communication).', syntax: 'tau_route_cost(depth, gear, tau_gate, tau_comm)', params: '`depth`（折叠后深度）、`gear`（齿轮比）、`tau_gate`（单层门时间）、`tau_comm`（单次跨层通信时间）。', paramsEn: '`depth`, `gear`, `tau_gate` (per-layer gate time), `tau_comm` (per-hop communication time).', returns: '`double`', returnsEn: '`double`' },
    tau_route_price: { purpose: '时间影子价格定价：净价值 v − λ·c；>0 表示值得在当前时间价格下调度。', purposeEn: 'Shadow price of time: net value v − λ·c; >0 means worth scheduling.', syntax: 'tau_route_price(value, cost, lambda)', params: '`value`（组件价值）、`cost`（组件时间成本）、`lambda`（时间影子价格）。', paramsEn: '`value`, `cost`, `lambda` (shadow price of time).', returns: '`double`', returnsEn: '`double`' },
    math_norm: { purpose: '向量 L2 范数 √(Σ a[i]²)。', purposeEn: 'Vector L2 norm √(Σ a[i]²).', syntax: 'math_norm(a, n)', params: '`a`（arr<double,N>）、`n`（int32）。', paramsEn: '`a` (arr<double,N>), `n` (int32).', returns: '`double`', returnsEn: '`double`' },
    complex: { purpose: '构造复数。', purposeEn: 'Constructs a complex number.', syntax: 'complex(re, im)', params: '`re`、`im`（double）实部 / 虚部。', paramsEn: '`re`, `im` (double) real / imaginary parts.', returns: '`complex128`', returnsEn: '`complex128`' },
    real: { purpose: '取复数实部。', purposeEn: 'Takes the real part of a complex number.', syntax: 'real(z)', returns: '`double`', returnsEn: '`double`' },
    imag: { purpose: '取复数虚部。', purposeEn: 'Takes the imaginary part.', syntax: 'imag(z)', returns: '`double`', returnsEn: '`double`' },
    cabs: { purpose: '取复数模长 |z|。', purposeEn: 'Takes the modulus |z|.', syntax: 'cabs(z)', returns: '`double`', returnsEn: '`double`' },
    conj: { purpose: '取复数共轭。', purposeEn: 'Takes the complex conjugate.', syntax: 'conj(z)', returns: '`complex128`', returnsEn: '`complex128`' },

    // ── QCOS 系统调用（syscall ABI）──
    rvdl_sys_call: { purpose: '系统调用（整数）。', purposeEn: 'System call (integer).', syntax: 'rvdl_sys_call(no, a, b, c)', returns: '`int32`', returnsEn: '`int32`' },
    rvdl_sys_calld: { purpose: '系统调用（double）。', purposeEn: 'System call (double).', syntax: 'rvdl_sys_calld(no, ...)', returns: '`double`', returnsEn: '`double`' },
    rvdl_sys_log: { purpose: '系统日志（字符串）。', purposeEn: 'System log (string).', syntax: 'rvdl_sys_log(level, msg)', returns: '`void`', returnsEn: '`void`' },
    rvdl_sys_logi: { purpose: '系统日志（整数）。', purposeEn: 'System log (integer).', syntax: 'rvdl_sys_logi(level, value)', returns: '`void`', returnsEn: '`void`' },
    rvdl_sys_callp: { purpose: '指针型系统调用入口。', purposeEn: 'Pointer-based system call entry.', syntax: 'rvdl_sys_callp(...)' },

    // ── QMS 数值内核 ──
    rvdl_qms_gap: { purpose: '量子 Markov 半群谱隙。', purposeEn: 'Quantum Markov semigroup spectral gap.', syntax: 'rvdl_qms_gap(model, p, q)', returns: '`double`', returnsEn: '`double`' },
    rvdl_mix_bound: { purpose: '混合界。', purposeEn: 'Mixing bound.', syntax: 'rvdl_mix_bound(gap, n, eps)', returns: '`double`', returnsEn: '`double`' },
    rvdl_qms_conc: { purpose: '方差收缩时间。', purposeEn: 'Variance contraction time.', syntax: 'rvdl_qms_conc(gap, eps)', returns: '`double`', returnsEn: '`double`' },

    // ── 系统级内建：原子 / 同步 ──
    sync_load: { purpose: '原子加载（load atomic）。', purposeEn: 'Atomic load.', syntax: 'sync_load(p)', returns: '`int32`', returnsEn: '`int32`' },
    sync_store: { purpose: '原子存储（store atomic）。', purposeEn: 'Atomic store.', syntax: 'sync_store(p, v)' },
    sync_add: { purpose: '原子加（atomicrmw add）。', purposeEn: 'Atomic add (atomicrmw add).', syntax: 'sync_add(p, v)', returns: '`int32`', returnsEn: '`int32`' },
    sync_cas: { purpose: '原子比较交换（cmpxchg）。', purposeEn: 'Atomic compare-and-swap (cmpxchg).', syntax: 'sync_cas(p, old, new)', returns: '`int32`', returnsEn: '`int32`' },
    sync_lock: { purpose: '原子 TAS 获取自旋锁（返回旧值：0 成功 / 1 已持有）。', purposeEn: 'Atomic TAS spinlock acquire (returns old value: 0 success / 1 held).', syntax: 'sync_lock(p)', returns: '`int32`', returnsEn: '`int32`' },
    sync_unlock: { purpose: '原子释放自旋锁。', purposeEn: 'Atomic spinlock release.', syntax: 'sync_unlock(p)' },

    // ── 系统级内建：volatile / 端口 I/O ──
    volatile_load: { purpose: 'volatile 加载（MMIO 轮询不被优化）。', purposeEn: 'Volatile load (MMIO polling not optimized away).', syntax: 'volatile_load(p)' },
    volatile_store: { purpose: 'volatile 存储。', purposeEn: 'Volatile store.', syntax: 'volatile_store(p, v)' },
    outb: { purpose: '8 位端口输出。', purposeEn: '8-bit port output.', syntax: 'outb(port, val)' },
    inb: { purpose: '8 位端口输入。', purposeEn: '8-bit port input.', syntax: 'inb(port)', returns: '`int32`', returnsEn: '`int32`' },
    outw: { purpose: '16 位端口输出。', purposeEn: '16-bit port output.', syntax: 'outw(port, val)' },
    inw: { purpose: '16 位端口输入。', purposeEn: '16-bit port input.', syntax: 'inw(port)', returns: '`int32`', returnsEn: '`int32`' },
    outl: { purpose: '32 位端口输出。', purposeEn: '32-bit port output.', syntax: 'outl(port, val)' },
    inl: { purpose: '32 位端口输入。', purposeEn: '32-bit port input.', syntax: 'inl(port)', returns: '`int32`', returnsEn: '`int32`' },

    // ── 系统级内建：寄存器 / MSR / CPUID ──
    read_cr0: { purpose: '读控制寄存器 CR0。', purposeEn: 'Reads control register CR0.', syntax: 'read_cr0()', returns: '`uint64`', returnsEn: '`uint64`' },
    read_cr2: { purpose: '读控制寄存器 CR2。', purposeEn: 'Reads control register CR2.', syntax: 'read_cr2()', returns: '`uint64`', returnsEn: '`uint64`' },
    read_cr3: { purpose: '读控制寄存器 CR3（页表根）。', purposeEn: 'Reads control register CR3 (page-table root).', syntax: 'read_cr3()', returns: '`uint64`', returnsEn: '`uint64`' },
    read_cr4: { purpose: '读控制寄存器 CR4。', purposeEn: 'Reads control register CR4.', syntax: 'read_cr4()', returns: '`uint64`', returnsEn: '`uint64`' },
    write_cr0: { purpose: '写控制寄存器 CR0。', purposeEn: 'Writes control register CR0.', syntax: 'write_cr0(v)' },
    write_cr3: { purpose: '写控制寄存器 CR3。', purposeEn: 'Writes control register CR3.', syntax: 'write_cr3(v)' },
    invlpg: { purpose: '刷新 TLB 单页。', purposeEn: 'Invalidates a single TLB page.', syntax: 'invlpg(addr)' },
    rdmsr: { purpose: '读模型特定寄存器（EDX:EAX 组合为 uint64）。', purposeEn: 'Reads a model-specific register (EDX:EAX combined into uint64).', syntax: 'rdmsr(msr)', returns: '`uint64`', returnsEn: '`uint64`' },
    wrmsr: { purpose: '写模型特定寄存器。', purposeEn: 'Writes a model-specific register.', syntax: 'wrmsr(msr, v)' },
    cpuid: { purpose: '探测 CPU 特性，写 4 个输出指针。', purposeEn: 'Probes CPU features, writes 4 output pointers.', syntax: 'cpuid(leaf, subleaf, eax, ebx, ecx, edx)' },
    rdtsc: { purpose: '读时间戳计数器 TSC。', purposeEn: 'Reads the timestamp counter TSC.', syntax: 'rdtsc()', returns: '`uint64`', returnsEn: '`uint64`' },
    read_rflags: { purpose: '读 RFLAGS。', purposeEn: 'Reads RFLAGS.', syntax: 'read_rflags()', returns: '`uint64`', returnsEn: '`uint64`' },
    xgetbv: { purpose: '读 XCR（如 XCR0 FPU 状态）。', purposeEn: 'Reads XCR (e.g. XCR0 FPU state).', syntax: 'xgetbv(xcr)', returns: '`uint64`', returnsEn: '`uint64`' },

    // ── 系统级内建：堆分配 / 取地址 ──
    rvdl_gc_alloc: { purpose: '内核堆分配。', purposeEn: 'Kernel heap allocation.', syntax: 'rvdl_gc_alloc(n)', params: '`n` 字节数。', paramsEn: '`n` byte count.', returns: '`cap<T>`', returnsEn: '`cap<T>`' },
    rvdl_gc_free: { purpose: '内核堆释放。', purposeEn: 'Kernel heap free.', syntax: 'rvdl_gc_free(p)' },
    addr: { purpose: '取函数地址（低 32 位）。', purposeEn: 'Takes a function\'s address (low 32 bits).', syntax: 'addr(&fn)', returns: '`int32`', returnsEn: '`int32`' },

    // ── 其他内建 ──
    strlen: { purpose: '字符串长度。', purposeEn: 'String length.', syntax: 'strlen(s)', returns: '`int32`', returnsEn: '`int32`' },
    kglobals_addr: { purpose: '内核全局状态区地址。', purposeEn: 'Kernel global-state region address.', syntax: 'kglobals_addr()' },

    // ── 量子通道逆因果容量（回程能力）──
    retrocausal_imax: { purpose: '量子通道的 max-information（Choi 矩阵最大特征值的对数）。', purposeEn: 'Max-information of a quantum channel (log of the largest Choi eigenvalue).', syntax: 'retrocausal_imax(kind, p)', params: '`kind`（int32）预置信道类型 0=去极化 1=退相 2=比特翻转 3=振幅阻尼 4=Hadamard 5=Y 翻转；`p`（double）信道参数。', paramsEn: '`kind` (int32) preset channel 0=depolarizing 1=dephasing 2=bit-flip 3=amplitude-damping 4=Hadamard 5=bit-phase-flip; `p` (double) channel parameter.', returns: '`double`', returnsEn: '`double`' },
    retrocausal_idoe: { purpose: '量子通道的 Doeblin information。', purposeEn: 'Doeblin information of a quantum channel.', syntax: 'retrocausal_idoe(kind, p)', returns: '`double`', returnsEn: '`double`' },
    retrocausal_q_capacity: { purpose: '渐近逆因果量子容量 Q_retro = ½(Imax + Idoe^∞)。', purposeEn: 'Asymptotic retrocausal quantum capacity Q_retro = ½(Imax + Idoe^∞).', syntax: 'retrocausal_q_capacity(kind, p)', returns: '`double`', returnsEn: '`double`' },
    retrocausal_c_capacity: { purpose: '渐近逆因果经典容量 C_retro = Imax + Idoe^∞。', purposeEn: 'Asymptotic retrocausal classical capacity C_retro = Imax + Idoe^∞.', syntax: 'retrocausal_c_capacity(kind, p)', returns: '`double`', returnsEn: '`double`' },
    retrocausal_q_one_shot: { purpose: '单次逆因果量子容量（含误差 ε）。', purposeEn: 'One-shot retrocausal quantum capacity (with error ε).', syntax: 'retrocausal_q_one_shot(kind, p, eps)', params: '`eps`（double）误差容限。', paramsEn: '`eps` (double) error tolerance.', returns: '`double`', returnsEn: '`double`' },
    retrocausal_gain: { purpose: '回程增益 Q_retro / Q_forward（时间不对称性）。', purposeEn: 'Retrocausal gain Q_retro / Q_forward (time asymmetry).', syntax: 'retrocausal_gain(kind, p)', returns: '`double`', returnsEn: '`double`' },
    retrocausal_deformed: { purpose: 'q-变形逆因果容量（q ∈ [−1,1] 参数化因果强度）。', purposeEn: 'q-deformed retrocausal capacity (q ∈ [−1,1] parameterizes causal strength).', syntax: 'retrocausal_deformed(kind, p, q)', params: '`q`（double）因果强度 −1=纯前向 +1=纯回程。', paramsEn: '`q` (double) causal strength −1=forward +1=retro.', returns: '`double`', returnsEn: '`double`' },
    retrocausal_ctc_q_capacity: { purpose: '快子 KK 双空间嘈杂 CTC（n 个 DD 维度）逆因果量子容量。', purposeEn: 'Retrocausal quantum capacity over noisy CTCs on n tachyon KK doubled dimensions.', syntax: 'retrocausal_ctc_q_capacity(n, theta, mu2, lambda)', params: '`n`（int32）额外维数；`theta`（double）Wilson line；`mu2`/`lambda`（double）快子势 V=−½μ²φ²+¼λφ⁴。', paramsEn: '`n` (int32) extra-dim count; `theta` (double) Wilson line; `mu2`/`lambda` (double) tachyon potential.', returns: '`double`', returnsEn: '`double`' },
    retrocausal_ctc_c_capacity: { purpose: '快子 KK 双空间嘈杂 CTC（n 个 DD 维度）逆因果经典容量。', purposeEn: 'Retrocausal classical capacity over noisy CTCs on n tachyon KK doubled dimensions.', syntax: 'retrocausal_ctc_c_capacity(n, theta, mu2, lambda)', returns: '`double`', returnsEn: '`double`' },
    retrocausal_ctc_gain: { purpose: '快子 KK 双空间嘈杂 CTC 的回程增益（Q_retro/Q_forward）。', purposeEn: 'Retrocausal gain (Q_retro/Q_forward) over tachyon KK noisy CTCs.', syntax: 'retrocausal_ctc_gain(n, theta, mu2, lambda)', returns: '`double`', returnsEn: '`double`' },
    retrocausal_ctc_dephasing: { purpose: 'Wilson line 破坏 KK ±n 对称导致的退相噪声（几何→噪声映射）。', purposeEn: 'Dephasing noise from Wilson-line KK ±n asymmetry (geometry→noise map).', syntax: 'retrocausal_ctc_dephasing(theta)', returns: '`double`', returnsEn: '`double`' },

    // ── QChain 量子区块链 ──
    qchain_wallet: { purpose: '创建量子区块链钱包。', purposeEn: 'Creates a quantum blockchain wallet.', syntax: 'qchain_wallet(...)' },
    qchain_mint: { purpose: '铸造代币。', purposeEn: 'Mints tokens.', syntax: 'qchain_mint(...)' },
    qchain_transfer: { purpose: '转账。', purposeEn: 'Transfers tokens.', syntax: 'qchain_transfer(...)' },
    qchain_balance: { purpose: '查询余额。', purposeEn: 'Queries balance.', syntax: 'qchain_balance(...)' },
    qchain_mine: { purpose: '挖矿。', purposeEn: 'Mines.', syntax: 'qchain_mine(...)' },
    qchain_height: { purpose: '查询区块高度。', purposeEn: 'Queries block height.', syntax: 'qchain_height()' },
    qchain_verify: { purpose: '验证链。', purposeEn: 'Verifies the chain.', syntax: 'qchain_verify(...)' },
    qchain_qkd: { purpose: '量子密钥分发。', purposeEn: 'Quantum key distribution.', syntax: 'qchain_qkd(...)' },
    qchain_qdba: { purpose: '量子数据库。', purposeEn: 'Quantum database.', syntax: 'qchain_qdba(...)' },
    qchain_coin_mint: { purpose: '铸造币。', purposeEn: 'Mints a coin.', syntax: 'qchain_coin_mint(...)' },
    qchain_coin_verify: { purpose: '验证币。', purposeEn: 'Verifies a coin.', syntax: 'qchain_coin_verify(...)' },
    qchain_sha3: { purpose: 'SHA-3 哈希。', purposeEn: 'SHA-3 hash.', syntax: 'qchain_sha3(...)' },
    qchain_hmac: { purpose: 'HMAC。', purposeEn: 'HMAC.', syntax: 'qchain_hmac(...)' },
    qchain_hash_unicode: { purpose: 'Unicode 哈希。', purposeEn: 'Unicode hash.', syntax: 'qchain_hash_unicode(...)' },
    qchain_sign: { purpose: '签名。', purposeEn: 'Signs.', syntax: 'qchain_sign(...)' },
    qchain_sign_verify: { purpose: '验证签名。', purposeEn: 'Verifies a signature.', syntax: 'qchain_sign_verify(...)' },
    qchain_sign_pubkey: { purpose: '获取签名公钥。', purposeEn: 'Gets the signing public key.', syntax: 'qchain_sign_pubkey(...)' },
    qchain_mlkem_encaps: { purpose: 'ML-KEM 封装。', purposeEn: 'ML-KEM encapsulation.', syntax: 'qchain_mlkem_encaps(...)' },
    qchain_mlkem_decaps: { purpose: 'ML-KEM 解封装。', purposeEn: 'ML-KEM decapsulation.', syntax: 'qchain_mlkem_decaps(...)' },
    qchain_causal_verify: { purpose: '因果验证。', purposeEn: 'Causal verification.', syntax: 'qchain_causal_verify(...)' },
    qchain_cipher_encrypt: { purpose: '加密。', purposeEn: 'Encrypts.', syntax: 'qchain_cipher_encrypt(...)' },
    qchain_cipher_decrypt: { purpose: '解密。', purposeEn: 'Decrypts.', syntax: 'qchain_cipher_decrypt(...)' },
};

/** 注解标签（@[...] 与 @layer(...)） */
export const ANNOTATION_DOCS: Record<string, HoverDoc> = {
    '@layer': {
        purpose: '多维拓扑调度标签（函数入口声明）。',
        purposeEn: 'Multi-dimensional topology scheduling tag (function entry declaration).',
        syntax: '@layer(time=T, thread=W, coord=(x,y,...)[, cost=N][, deadline=D]\n'
            + '       [, shuttle=(cx,cy,...)][, track=(obs,...)])',
        params: '`time` 锚点时间（根块必填）、`thread` 逻辑线程 id、`coord` N 维坐标；'
            + '`cost`（默认 1）、`deadline` 时间束缚上限、`dual` 对偶标记。\n\n'
            + '两个可选机制（默认**完全惰性** —— 不写则与既有行为逐字节相同）：\n'
            + '- **穿梭** `shuttle` / `shuttle_lambda` / `shuttle_phase` / `shuttle_rounds` / `shuttle_strategy`：'
            + '按关联度在共享点前后双向渗透。\n'
            + '- **追踪** `track` / `track_mode` / `track_slots`：周期内把可观测量转成经典影子再恢复。\n\n'
            + '悬停到具体键名或枚举取值可查看该键的语义与取值域。',
        paramsEn: '`time` anchor time (required for root blocks), `thread` logical thread id, `coord` N-dim coordinates; '
            + '`cost` (default 1), `deadline` time-bound upper limit, `dual` duality flag.\n\n'
            + 'Two optional mechanisms (both **fully inert** by default — omitting them keeps existing behaviour byte-identical):\n'
            + '- **Shuttle** `shuttle` / `shuttle_lambda` / `shuttle_phase` / `shuttle_rounds` / `shuttle_strategy`: '
            + 'bidirectional weighted permeation around the shared point.\n'
            + '- **Tracking** `track` / `track_mode` / `track_slots`: convert observables to classical shadows for the period, then restore.\n\n'
            + 'Hover a specific key or enum value for its semantics and domain.',
        example: '@layer(time=0, thread=0, coord=(0,0), shuttle=(1,0), track=(expect_z, entropy))\nint32 main() { return 0; }',
    },
    '@[gate]': { purpose: '标记为可组合量子门单元。', purposeEn: 'Marks a composable quantum gate unit.', syntax: '@[gate]\nvoid U(Qubit q) { ... }' },
    '@[undo]': { purpose: '合成可逆对偶 U†（门序反转 + 逐门取逆）→ `<name>_undo`。', purposeEn: 'Synthesizes the reversible dual U† (reversed gate order + per-gate inverse) → `<name>_undo`.', syntax: '@[gate] @[undo]\nvoid U(Qubit q) { ... }' },
    '@[steer]': { purpose: '合成相干控制版本 Λ(U)（控制位导引目标门）→ `<name>_steer`。', purposeEn: 'Synthesizes the coherent controlled version Λ(U) (control guides the target gate) → `<name>_steer`.', syntax: '@[gate] @[steer]\nvoid U(Qubit q) { ... }' },
    '@[unitary]': { purpose: '酉性验证（无测量、无经典分支依赖）。', purposeEn: 'Unitarity verification (no measurement, no classical branch dependence).', syntax: '@[unitary]\nvoid U(Qubit q) { ... }' },
    '@[measure]': { purpose: '标记测量点（消费 Qubit，纳入 QLT 线性消费）。', purposeEn: 'Marks a measurement point (consumes Qubit, joins QLT linear consumption).', syntax: '@[measure]\nvoid M(Qubit q) { ... }' },
    '@[coherence]': { purpose: '相干时间（T1 弛豫 / T2 退相，单位 μs；须 0 < T2 ≤ T1）。', purposeEn: 'Coherence time (T1 relaxation / T2 dephasing, in μs; requires 0 < T2 ≤ T1).', syntax: '@[coherence(100, 50)]' },
    '@[noise]': { purpose: '噪声模型（`depolarizing`/`amplitude_damping`/`phase_damping`/`bit_flip`）。', purposeEn: 'Noise model (`depolarizing`/`amplitude_damping`/`phase_damping`/`bit_flip`).', syntax: '@[noise("depolarizing")]' },
    '@[basis]': { purpose: '指定测量基（X/Y/Z）。', purposeEn: 'Specifies the measurement basis (X/Y/Z).', syntax: '@[basis("X")]' },
    '@[decoherence_free]': { purpose: '无退相干子空间（DFS）。', purposeEn: 'Decoherence-free subspace (DFS).', syntax: '@[decoherence_free]' },
    '@[error_correction]': { purpose: '纠错码（如 `surface`）。', purposeEn: 'Error-correcting code (e.g. `surface`).', syntax: '@[error_correction("surface")]' },
    '@[inline]': { purpose: '强制内联。', purposeEn: 'Force inline.', detail: 'LLVM 映射：`alwaysinline`。', detailEn: 'LLVM mapping: `alwaysinline`.' },
    '@[noinline]': { purpose: '禁止内联。', purposeEn: 'Disable inline.', detail: 'LLVM 映射：`noinline`。', detailEn: 'LLVM mapping: `noinline`.' },
    '@[pure]': { purpose: '无副作用。', purposeEn: 'No side effects.', detail: 'LLVM 映射：`readnone`。', detailEn: 'LLVM mapping: `readnone`.' },
    '@[readonly]': { purpose: '只读。', purposeEn: 'Read-only.', detail: 'LLVM 映射：`readonly`。', detailEn: 'LLVM mapping: `readonly`.' },
    '@[cold]': { purpose: '冷路径提示。', purposeEn: 'Cold-path hint.', detail: 'LLVM 映射：`cold`。', detailEn: 'LLVM mapping: `cold`.' },
    '@[hot]': { purpose: '热路径提示。', purposeEn: 'Hot-path hint.', detail: 'LLVM 映射：`hot`。', detailEn: 'LLVM mapping: `hot`.' },
    '@[noreturn]': { purpose: '函数不返回。', purposeEn: 'Function does not return.', detail: 'LLVM 映射：`noreturn`。', detailEn: 'LLVM mapping: `noreturn`.' },
    '@[export]': { purpose: '导出符号。', purposeEn: 'Exports symbol.', detail: 'LLVM 映射：`dllexport`。', detailEn: 'LLVM mapping: `dllexport`.' },
    '@[section]': { purpose: '段放置（如 `@[section(".text.boot")]`）。', purposeEn: 'Section placement (e.g. `@[section(".text.boot")]`).', syntax: '@[section(".text.boot")]' },
    '@[naked]': { purpose: '裸函数（无栈帧、无 prologue/epilogue）。', purposeEn: 'Naked function (no stack frame, no prologue/epilogue).', syntax: '@[naked]\nvoid f() { asm { "iretq;" } }' },
};

// ═══════════════════════════════════════════════════════════════════
// @layer(...) 的键文档
//
// 单一事实来源：`LAYER_TAG_KEYS` 同时驱动
//   ① 悬停（悬停键名 → 语义与取值域）
//   ② `@layer(...)` 内的上下文补全（键位只给键）
//   ③ 枚举键的取值悬停（如 `shuttle_strategy=3` 悬停到 3）
//
// 之所以把三处收进一张表：新增键时若只改补全不改悬停，用户会「补得出来、
// 看不懂」，反之则会「看得懂、写不出」。两张表各自忠实于自己、合起来是错的
// —— 这是一类不会报错的漂移。
//
// 见 docs/rvdl-shuttle-tracking.md。
// ═══════════════════════════════════════════════════════════════════

/** 键名（补全用）。顺序即补全中的展示顺序：既有键在前，新机制键在后。 */
export const LAYER_TAG_KEY_NAMES = [
    'time', 'thread', 'coord', 'cost', 'deadline', 'dual',
    'shuttle', 'shuttle_lambda', 'shuttle_phase', 'shuttle_rounds', 'shuttle_strategy',
    'track', 'track_mode', 'track_slots',
] as const;

/** 枚举取值说明（悬停到取值时展示）。键为 `@layer` 键名，值为「取值 → 说明」。 */
export const LAYER_ENUM_VALUES: Record<string, Record<string, { zh: string; en: string }>> = {
    shuttle_phase: {
        '0': { zh: '仅共享**前**：目标层入口投递（目标把关联当输入看到）。', en: 'Before only: delivered at target-layer entry (target sees the correlation as input).' },
        '1': { zh: '仅共享**后**：目标层出口的返回腿。', en: 'After only: return leg at target-layer exit.' },
        '2': { zh: '**前后都渗透**（默认）：两个传播锥的并集，每个关联层同时是源与汇。', en: 'Both (default): union of the two propagation cones; every related layer is both source and sink.' },
    },
    shuttle_strategy: {
        '-1': { zh: '`auto`（默认）：运行期按对端数 / 载荷 / ancilla / 格点跨度启发式选择，**决策写入运行诊断**，不静默。', en: '`auto` (default): chosen at runtime from peer count / payload / ancilla / lattice span; the decision is written to runtime diagnostics.' },
        '0': { zh: '**蝶式**：深度 O(log span)，全体并行置换。', en: 'Butterfly: depth O(log span), all-to-all parallel permutation.' },
        '1': { zh: '**环式**：深度 O(N)，最近邻。大消息 / 层级集群下可能反超蝶式（带宽项相同）。', en: 'Ring: depth O(N), nearest-neighbour. Can beat butterfly for large messages or tiered clusters.' },
        '2': { zh: '**树式**：深度 O(log N)，树汇聚 / 散播。', en: 'Tree: depth O(log N), tree gather/scatter.' },
        '3': { zh: '**Grover 扩散**：深度 O(1)，代价是 2ⁿ 工作寄存器；缺寄存器时报 `W-TRK001` 并降级为 0。', en: 'Grover diffusion: depth O(1) at the cost of a 2ⁿ work register; without it `W-TRK001` fires and it degrades to 0.' },
        '4': { zh: '**c001 蝶点**：全体关联层的主要特征**压缩成点（蝶点）再辐射**。深度恒 1 轮（与 span / 对端数无关），零附加资源。多个蝶点经拓扑边互联成「DNA 链」多层架构。', en: '**c001 butterfly-point**: every related layer\'s dominant features are **compressed into a single point (butterfly point) and then radiated out**. Depth is always 1 round (independent of span / peer count) with zero extra resources. Butterfly points linked by topology edges form a "DNA-chain" multi-layer architecture.' },
    },
    track_mode: {
        '0': { zh: '`auto`（默认）：按可观测量所属族自动选级 —— Pauli→L0、经典量→L2、其余→L1。', en: '`auto` (default): level chosen from the observable family — Pauli→L0, classical→L2, else→L1.' },
        '1': { zh: '强制 **L0 / qnd**：非破坏读出，保真度 = 1。仅 Pauli 族可实现，其余报 `E-TRK003`。', en: 'Force L0/qnd: non-disturbing readout, fidelity = 1. Only realisable for the Pauli family; otherwise `E-TRK003`.' },
        '2': { zh: '强制 **L1 / shadow**：经典阴影，统计级保真度（采样收敛）。', en: 'Force L1/shadow: classical shadows, statistical fidelity.' },
        '3': { zh: '强制 **L2 / relabel**：态从未被触碰，保真度 = 1。', en: 'Force L2/relabel: the state is never touched, fidelity = 1.' },
    },
};

/** 各键的插入文本（补全用）。类型强制对 `LAYER_TAG_KEY_NAMES` 穷尽 —— 少一个键就是编译错误。 */
export const LAYER_TAG_INSERTS: Record<(typeof LAYER_TAG_KEY_NAMES)[number], string> = {
    time: 'time=',
    thread: 'thread=',
    coord: 'coord=(',
    cost: 'cost=',
    deadline: 'deadline=',
    dual: 'dual=',
    shuttle: 'shuttle=(',
    shuttle_lambda: 'shuttle_lambda=',
    shuttle_phase: 'shuttle_phase=',
    shuttle_rounds: 'shuttle_rounds=',
    shuttle_strategy: 'shuttle_strategy=',
    track: 'track=(',
    track_mode: 'track_mode=',
    track_slots: 'track_slots=',
};

export const LAYER_KEY_DOCS: Record<(typeof LAYER_TAG_KEY_NAMES)[number], HoverDoc> = {
    time: {
        purpose: '锚点时间（分层排序的第一关键字）。',
        purposeEn: 'Anchor time (primary sort key for layering).',
        detail: '同一时间层内的块可并行；不同时间层严格串行。功能周期为 `[time, time+cost)`。取负值报 `E-TOP009`。',
        detailEn: 'Blocks in the same time layer may run in parallel; layers run strictly in order. The functional period is `[time, time+cost)`. Negative values raise `E-TOP009`.',
        syntax: '@layer(time=0, thread=0, coord=(0,0))',
    },
    thread: {
        purpose: '逻辑线程 id（同 coord 不同线程构成 crossing 边）。',
        purposeEn: 'Logical thread id (same coord, different thread forms a crossing edge).',
        detail: '取负值报 `E-TOP009`。',
        detailEn: 'Negative values raise `E-TOP009`.',
    },
    coord: {
        purpose: 'N 维坐标（关联图的格点地址）。',
        purposeEn: 'N-dimensional coordinate (lattice address in the relation graph).',
        detail: '维度无需一致（原 `E-TOP002` 已放宽，跨维度投影路径缺失时报 `E-TOP007`）。'
            + 'coord 是格点**地址**而非计数，**允许负数**；关联度用 L1 距离 `Σᵢ|aᵢ−bᵢ|`，对负地址同样良定义。',
        detailEn: 'Dimensions need not agree (the old `E-TOP002` was relaxed; a missing cross-dimension projection path raises `E-TOP007`). '
            + 'coord is a lattice **address**, not a count, so negatives are allowed; the correlation uses the L1 distance `Σᵢ|aᵢ−bᵢ|`, well defined for negative addresses too.',
        syntax: '@layer(time=0, thread=0, coord=(0,1))',
    },
    cost: {
        purpose: '功能周期长度（默认 1）。',
        purposeEn: 'Functional period length (default 1).',
        detail: '`[time, time+cost)` 也是 `@track` 影子槽的生命周期。取负值报 `E-TOP009`（负 cost 会让周期反向）。',
        detailEn: '`[time, time+cost)` is also the lifetime of `@track` shadow slots. Negative values raise `E-TOP009`.',
    },
    deadline: {
        purpose: '时间束缚上限（`E-TOP006` / `E-SHU007` 依据）。',
        purposeEn: 'Time-bound upper limit (basis for `E-TOP006` / `E-SHU007`).',
        detail: '规划出的穿梭轮次会使执行超出 deadline 时报 `E-SHU007`。取负值报 `E-TOP009`。',
        detailEn: '`E-SHU007` fires when the planned shuttle rounds push execution past the deadline. Negative values raise `E-TOP009`.',
    },
    dual: {
        purpose: '对偶标记（T-对偶反转空间；与 `stack`/`crossing` 边正交）。',
        purposeEn: 'Dual flag (T-duality reversed space; orthogonal to `stack`/`crossing` edges).',
        detail: '同 coord 且 dual 不同 → inversion 边。接受 `1`/`0`，也接受 `true`/`false`。',
        detailEn: 'Same coord with differing dual → inversion edge. Accepts `1`/`0` as well as `true`/`false`.',
    },
    shuttle: {
        purpose: '**穿梭**：声明目标 coord，把本块的数据按关联度在共享点前后双向渗透进关联层。',
        purposeEn: '**Shuttle**: declare target coords; permeate this block\'s data into related layers, weighted by correlation, on both sides of the shared point.',
        detail: '关联度核 `w = exp(−d_L1/λ)` **不设任何截断阈值**（`w > 0` 恒成立）——与注意力 softmax 根本不同。'
            + '线格式同时下发 `weight` 与 `logWeight`：`d/λ ≳ 746` 时线性权重会双精度下溢为 0，'
            + '但路由一律用 `logWeight`，对端**仍被调度**（数值零 ≠ 语义截断）。'
            + '目标 coord 无块报 `E-SHU001`；无可用对端报 `E-SHU002`；'
            + '无 `@track` 则量子相干路径无影子供给，报 `W-SHU001` 并退化为经典影子混合。',
        detailEn: 'The kernel `w = exp(−d_L1/λ)` has **no truncation threshold** (`w > 0` always) — fundamentally unlike attention softmax. '
            + 'The wire format sends both `weight` and `logWeight`: for `d/λ ≳ 746` the linear form underflows to 0 in double, '
            + 'but routing always uses `logWeight` so peers are **still scheduled** (numeric zero ≠ semantic truncation). '
            + 'No match raises `E-SHU001`; no admissible peer raises `E-SHU002`; '
            + 'without `@track` the coherent path has no shadow supply, raising `W-SHU001` and degrading to classical shadow mixing.',
        syntax: '@layer(time=0, thread=0, coord=(0,0), shuttle=(1,0), track=(expect_z))\nint32 producer() { return 0; }',
    },
    shuttle_lambda: {
        purpose: '穿梭关联度核的尺度 λ（默认 1，必须 > 0）。',
        purposeEn: 'Scale λ of the shuttle correlation kernel (default 1, must be > 0).',
        detail: 'λ 越大，关联衰减越慢（关联半径越大）。`λ ≤ 0` 报 `E-SHU003` —— 指数核必须严格正。',
        detailEn: 'Larger λ means slower decay (larger correlation radius). `λ ≤ 0` raises `E-SHU003` — the kernel must be strictly positive.',
        syntax: '@layer(..., shuttle=(1,0), shuttle_lambda=0.5)',
    },
    shuttle_phase: {
        purpose: '渗透相对**目标层** time-slot 的时机（默认 2）。',
        purposeEn: 'Timing of permeation relative to the **target layer** time-slot (default 2).',
        detail: '取值域 `{0, 1, 2}`，其余报 `E-SHU004`。`2` 是双侧恢复算子，覆盖面严格大于单侧。',
        detailEn: 'Domain `{0, 1, 2}`; anything else raises `E-SHU004`. `2` is the two-sided recovery operator, strictly wider than either side.',
        syntax: '@layer(..., shuttle=(1,0), shuttle_phase=2)',
    },
    shuttle_rounds: {
        purpose: '渗透轮次（`0` = auto）。',
        purposeEn: 'Permeation rounds (`0` = auto).',
        detail: 'auto 取 `⌈log₂ span⌉`，`span = ∏ᵢ(coord_max[i] − min(0, coord_min[i]) + 1)`。'
            + '取负值报 `E-SHU006`；轮次导致超出 deadline 报 `E-SHU007`。',
        detailEn: 'auto means `⌈log₂ span⌉` with `span = ∏ᵢ(coord_max[i] − min(0, coord_min[i]) + 1)`. '
            + 'Negative values raise `E-SHU006`; rounds that overrun the deadline raise `E-SHU007`.',
        syntax: '@layer(..., shuttle=(1,0), shuttle_rounds=3)',
    },
    shuttle_strategy: {
        purpose: '穿梭策略（`-1` = auto，默认）。',
        purposeEn: 'Shuttle strategy (`-1` = auto, the default).',
        detail: '取值域 `{-1, 0, 1, 2, 3, 4}`，其余报 `E-SHU005`。'
            + '深度：蝶式 `O(log span)`、环式 `O(N)`、树式 `O(log N)`、Grover `O(1)`、'
            + 'c001 蝶点 `O(1)`（恒 1 轮，零附加资源）。'
            + 'Grover 需要 2ⁿ 工作寄存器，缺失时报 `W-TRK001` 并降级为 `0`；'
            + 'c001 蝶点无资源需求，把全体层主要特征压缩成点再辐射。',
        detailEn: 'Domain `{-1, 0, 1, 2, 3, 4}`; anything else raises `E-SHU005`. '
            + 'Depth: butterfly `O(log span)`, ring `O(N)`, tree `O(log N)`, Grover `O(1)`, '
            + 'c001 butterfly-point `O(1)` (always 1 round, zero extra resources). '
            + 'Grover needs a 2ⁿ work register; without it `W-TRK001` fires and it degrades to `0`; '
            + 'the c001 point needs none — it compresses every layer\'s dominant features into one point and radiates back.',
        syntax: '@layer(..., shuttle=(1,0), shuttle_strategy=4)',
    },
    track: {
        purpose: '**追踪**：在块的功能周期内把可观测量暂时转成经典影子并复制，再把原态恢复。',
        purposeEn: '**Tracking**: within the block\'s functional period, temporarily convert observables to classical shadows, copy them, then restore the original state.',
        detail: '四段式 `EXTRACT → COPY → RESTORE → 释放`。'
            + '**单次软克隆在物理上不可能**，故采用三级保真阶梯（见 `track_mode`），不做统一精确承诺。'
            + '影子槽只承载**经典标量**（结构上不可能是 `QObject`）——这正是保 QLT 线性不变量 '
            + '`E-Q001`（no-cloning）/ `E-Q002`（线性泄漏）的方式。'
            + '空列表或 `track_slots < 1` 报 `E-TRK001`；未知可观测量报 `E-TRK003`。',
        detailEn: 'Four stages: `EXTRACT → COPY → RESTORE → release`. '
            + 'A single-shot soft clone is physically impossible, hence the three-level fidelity ladder (see `track_mode`) with no uniform exactness promise. '
            + 'Shadow slots hold **classical scalars only** (structurally cannot be `QObject`) — this is exactly how the QLT invariants '
            + '`E-Q001` (no-cloning) / `E-Q002` (linear leak) are preserved. '
            + 'An empty list or `track_slots < 1` raises `E-TRK001`; an unknown observable raises `E-TRK003`.',
        syntax: '@layer(time=0, thread=0, coord=(0,0), track=(expect_z, entropy), track_slots=2)',
    },
    track_mode: {
        purpose: '追踪保真级别（`0` = auto，默认）。',
        purposeEn: 'Tracking fidelity level (`0` = auto, the default).',
        detail: '取值域 `{0,1,2,3}`，其余报 `E-TRK004`。'
            + '非零值**强制覆盖** auto 选级（强制即强制，否则该键是哑参数）。'
            + 'L0 无采样数即无法构成阴影 → 作用域判定失效。',
        detailEn: 'Domain `{0,1,2,3}`; anything else raises `E-TRK004`. '
            + 'A non-zero value **overrides** auto level selection (otherwise the key would be a no-op). '
            + 'L0 without a sample count cannot form a shadow → the scope is marked invalid.',
    },
    track_slots: {
        purpose: '经典影子槽数（默认 1）。',
        purposeEn: 'Number of classical shadow slots (default 1).',
        detail: '槽位是块的局部变量，随功能周期 `[time, time+cost)` 释放 → 不构成持久克隆。'
            + '`< 1` 报 `E-TRK001`。',
        detailEn: 'Slots are local to the block and released with the functional period `[time, time+cost)` → not a persistent clone. '
            + '`< 1` raises `E-TRK001`.',
        syntax: '@layer(..., track=(expect_z), track_slots=4)',
    },
};

// ─── 安全索引入口 ───────────────────────────────────────────────────
// LAYER_KEY_DOCS 的键类型是字面量联合（编译期穷尽），因此不能用任意 string
// 索引。这两个函数是唯一的字符串索引入口，同时把「不是键」与「键无文档」
// 两种情形统一成 undefined，避免调用方各自写 hasOwnProperty。

/** 取 `@layer` 键的文档；`name` 不是键时返回 undefined。 */
export function layerKeyDoc(name: string): HoverDoc | undefined {
    return Object.prototype.hasOwnProperty.call(LAYER_KEY_DOCS, name)
        ? (LAYER_KEY_DOCS as Record<string, HoverDoc>)[name]
        : undefined;
}

/** 取 `@layer` 枚举键某个取值的说明；不存在时返回 undefined。 */
export function layerEnumValueDoc(key: string, value: string): { zh: string; en: string } | undefined {
    return Object.prototype.hasOwnProperty.call(LAYER_ENUM_VALUES, key)
        ? LAYER_ENUM_VALUES[key][value]
        : undefined;
}