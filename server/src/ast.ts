export interface ASTNode {
    type: string;
    line: number;
    column: number;
    length: number;
}

export interface Program extends ASTNode {
    type: 'Program';
    body: (Statement | Item)[];
}

export type Statement = 
    |VariableDeclaration 
    |VariableDeclarationList
    | ExpressionStatement 
    | WhileStatement 
    | ForStatement
    | IfStatement
    | RouteStatement
    | SpinStatement
    | UnsafeBlock
    | BreakStatement
    | ContinueStatement
    | AssignmentStatement
    | FunctionDeclaration 
    | SpawnStatement
    | EntangleStatement
    | AsmStatement
    | ReturnStatement;

/**
 * 条件分支。
 *
 * 注意：`else if` **不**单独设节点，而是把 `else` 分支表示为「只含一个
 * IfStatement 的语句数组」。这样 IR/MIR  lowering 只需处理一种分支结构，
 * 无需为链式 else-if 特判。
 */
export interface IfStatement extends ASTNode {
    type: 'IfStatement';
    condition: Expression;
    /** then 分支 */
    consequent: Statement[];
    /** else 分支；`else if` 时该数组只含一个 IfStatement；无 else 时为 null */
    alternate: Statement[] | null;
}

/**
 * unsafe 块：允许裸指针解引用、内联汇编、MMIO 等"系统级"操作。
 * 是系统级编程（写内核）的显式危险边界；capability 校验在语义层完成。
 */
export interface UnsafeBlock extends ASTNode {
    type: 'UnsafeBlock';
    body: Statement[];
}

/**
 * 路由分支（新范式 switch）：把一个判别值"路由"到若干路径之一。
 * 命名源自拓扑量子编译的 qubit routing。
 */
export interface RouteCase {
    /** 路径值（常量表达式，如数字/字符/布尔） */
    value: Expression;
    body: Statement[];
}

export interface RouteStatement extends ASTNode {
    type: 'RouteStatement';
    /** 被路由的判别表达式 */
    discriminant: Expression;
    /** 各 case 分支 */
    cases: RouteCase[];
    /** default 分支（fallback）；无则为 null */
    fallback: Statement[] | null;
}

/**
 * 自旋循环（新范式 do-while）：先执行循环体，再判断条件；至少执行一次。
 * 命名源自量子态的自旋演化。
 */
export interface SpinStatement extends ASTNode {
    type: 'SpinStatement';
    body: Statement[];
    condition: Expression;
}

export interface VariableDeclaration extends ASTNode {
    type: 'VariableDeclaration';
    varType: string;
    identifier: string;
    value: Expression;
    /** fixed：编译期常量，不可重新赋值（确定型范式） */
    isFixed?: boolean;
}

/**
 * 一行多变量声明（如 `int32 a = 1, b = 2, c = 3;`）。
 * 多个声明共享同一类型（`varType`）与 `fixed` 修饰符，各自携带独立的
 * 标识符与初始值。作为语句（局部作用域）或顶层 Item（全局变量列表）出现。
 */
export interface VariableDeclarationList extends ASTNode {
    type: 'VariableDeclarationList';
    varType: string;
    isFixed?: boolean;
    declarations: VariableDeclaration[];
}

export interface ExpressionStatement extends ASTNode {
    type: 'ExpressionStatement';
    expression: Expression;
}

export interface BinaryExpression  extends ASTNode {
    type: 'BinaryExpression';
    operator: string;
    left: Expression;
    right: Expression;
}

export interface LogicalExpression extends ASTNode {
    type: 'LogicalExpression';
    operator: '&&' | '||';
    left: Expression;
    right: Expression;
}

export interface UnaryExpression extends ASTNode {
    type: 'UnaryExpression';
    operator: '!' | '-';
    argument: Expression;
}

export interface ResultExpr extends ASTNode {
    type: 'ResultExpr';
}

export interface FunctionExpression extends ASTNode {
    type: 'FunctionExpression';
    params: Param[];
    returnType: string | null;
    body: Statement[];
}

export interface WhileStatement extends ASTNode {
    type: 'WhileStatement';
    condition: Expression;
    body: Statement[];
    elseBody?: Statement[];
    invariant?: Expression[];
}

export interface ForStatement extends ASTNode {
    type: 'ForStatement';
    init: Statement | null;
    condition: Expression | null;
    update: Statement | null;
    body: Statement[];
    invariant?: Expression[];
}

export interface BreakStatement extends ASTNode {
    type: 'BreakStatement';
}

export interface ContinueStatement extends ASTNode {
    type: 'ContinueStatement';
}

export interface AssignmentStatement extends ASTNode {
    type: 'AssignmentStatement';
    name: string;
    target?: Expression;
    value: Expression;
}

// ============================================================================
// 表达式
// ============================================================================
export type Expression =
    | NumberLiteral 
    | StringLiteral
    | CharLiteral
    | NullLiteral
    | Dereference
    | AddressOf
    | NativeExpression
    | FuseExpression
    | Identifier 
    | FunctionCall
    | NewExpression
    | MemberExpression
    | IndexExpression
    | BinaryExpression
    | LogicalExpression
    | UnaryExpression
    | ResultExpr
    | FunctionExpression
    | ArrayLiteral

export interface NumberLiteral extends ASTNode {
    type: 'NumberLiteral';
    value: number;
    /** 字面量源码中是否带小数点/指数（如 0.0、1e3），决定其应为 double 而非 int32 */
    isFloat?: boolean;
    /** 原始源码文本（如 0xFFFFFFFFFFFFFFFF / 18446744073709551615），保留 64 位精度 */
    raw?: string;
}

export interface CharLiteral extends ASTNode {
    type: 'CharLiteral';
    value: string;
}

export interface StringLiteral extends ASTNode {
    type: 'StringLiteral';
    value: string;
}

/**
 * 数组字面量：[e0, e1, ...]。
 * 用于静态查找表（键盘扫描码表 / 异常名表）与局部定长数组初始化。
 * 类型由上下文（arr<T, N> 声明）或元素类型推断。
 */
export interface ArrayLiteral extends ASTNode {
    type: 'ArrayLiteral';
    elements: Expression[];
}

/** null 指针字面量 */
export interface NullLiteral extends ASTNode {
    type: 'NullLiteral';
}

/** 指针解引用：*p */
export interface Dereference extends ASTNode {
    type: 'Dereference';
    target: Expression;
}

/** 取地址：&fn（函数指针，供中断处理函数 / 调度任务入口用） */
export interface AddressOf extends ASTNode {
    type: 'AddressOf';
    target: Expression;
}

/**
 * 原生指令：native("...", op1, op2, ...)——逃逸到目标机器的原生指令集（代数效应
 * 视角下的"原生效应"）。模板字符串用 ${0}/${1} 占位符引用操作数；操作数默认
 * 用 "r"（通用寄存器）约束，内存间接寻址在模板里写 `(${N})`（如 lidt (${0})）。
 *
 * 无操作数：native("cli") / native("iretq") / native("hlt")；
 * 带操作数：native("lidt (${0})", &idtr) / native("fxsave (${0})", buf) /
 *           native("mov %%rsp, ${0}", sp)。
 * 必须在 unsafe 块内（持有能力授予）方可使用。
 */
export interface NativeExpression extends ASTNode {
    type: 'NativeExpression';
    template: string;
    operands: Expression[];
}

/**
 * 融合-模式匹配表达式：把判别值"融合"到若干模式之一并求值。
 * 命名源自 Hopf 代数的余乘（comultiplication）——把一个值解构为分支。
 */
export interface FuseArm {
    /** 模式：常量表达式；null 表示通配符 _ */
    pattern: Expression | null;
    value: Expression;
}

export interface FuseExpression extends ASTNode {
    type: 'FuseExpression';
    discriminant: Expression;
    arms: FuseArm[];
}

export interface Identifier extends ASTNode {
    type: 'Identifier';
    name: string;
}

export interface FunctionCall extends ASTNode {
    type: 'FunctionCall';
    name: string;             // 简单名，或完整路径 'a::b::fn'
    arguments: Expression[];
}

export interface NewExpression extends ASTNode {
    type: 'NewExpression';
    className: string;
    arguments: Expression[];
    heapAlloc?: boolean;
}

export interface MemberExpression extends ASTNode {
    type: 'MemberExpression';
    object: Expression;
    property: string;
    isMethodCall: boolean;
    arguments: Expression[];
}

/**
 * 晶格索引：board[x, y] —— 对 lattice 的多维下标访问。
 * object 是 lattice 表达式，indices 是各维下标。
 * 作为表达式求值（读取元素）；作为赋值目标（AssignmentStatement.target）时写入。
 * 命名源自晶格（lattice）的格点坐标。
 */
export interface IndexExpression extends ASTNode {
    type: 'IndexExpression';
    object: Expression;
    indices: Expression[];
}

export type Item =
    | ModuleDecl
    | UseDecl
    | FormDecl
    | FlavorDecl
    | ImplDecl
    | TraitDecl
    | TemplateDecl
    | ImportDecl
    | RequiresDecl
    | ExternDecl
    | GlobalVarDecl
    | VariableDeclarationList;

/**
 * 顶层全局变量 / 常量。
 *   int32 counter = 0;       —— 全局可变变量
 *   fixed int32 N = 42;      —— 顶层编译期常量（确定型范式）
 * 命名对齐 shim.c 的「内核全局状态区」ABI：rvdl 侧的全局变量。
 */
export interface GlobalVarDecl extends ASTNode {
    type: 'GlobalVarDecl';
    name: string;
    varType: string;   // 类型（int32 / cap<T> / lattice<T,B> 等）
    isFixed: boolean;  // fixed 常量（编译期常量）
    value: Expression; // 初始值
    isPub: boolean;
}

export interface Param {
    name: string;
    type: string;
}

export interface FieldDecl {
    name: string;
    type: string;
    isPub: boolean;
    /** 位域宽度（`uint8 f : 3`），与同类型相邻位域打包进同一存储单元 */
    bitWidth?: number;
}


export interface FnDecl {
    name: string;
    receiver: string | null;
    params: Param[];
    returnType: string;
    isPub: boolean;
    body: Statement[] | null;
    line: number;
    column: number;
    length: number;
}

export interface RankBlock {
    name: string;
    fields: FieldDecl[];
    methods: FnDecl[];
}

export interface InheritClause {
    base: string;
    ranks: string[];
}

export interface ModuleDecl extends ASTNode {
    type: 'ModuleDecl';
    name: string;
    body: Item[];
}

export interface UseDecl extends ASTNode {
    type: 'UseDecl';
    path: string[];
}

export interface FormDecl extends ASTNode {
    type: 'FormDecl';
    name: string;
    isPub: boolean;
    isExport: boolean;
    inherits: InheritClause | null;
    ranks: RankBlock[];
    /** @[packed] 精确字节布局：无 vtable 指针、无填充、字段从偏移 0 起紧密排列 */
    packed?: boolean;
}

/**
 * 味：一组具名常量，成员自动按声明顺序赋整数值 0,1,2,...，
 * 也支持显式赋值（flavor Color { RED = 1, GREEN = 5, BLUE }）。
 * 命名源自量子味（flavor）概念。
 */
export interface FlavorMember {
    name: string;
    /** 显式整数值；null 表示按前一个成员值 +1 自动递增（首个默认为 0） */
    value: number | null;
}

export interface FlavorDecl extends ASTNode {
    type: 'FlavorDecl';
    name: string;
    members: FlavorMember[];
    isPub: boolean;
}

export interface ImplDecl extends ASTNode {
    type: 'ImplDecl';
    target: string;
    traitName: string | null;
    rank: string | null;
    methods: FnDecl[];
}

export interface TraitDecl extends ASTNode {
    type: 'TraitDecl';
    name: string;
    isPub: boolean;
    inherits: InheritClause | null;
    ranks: RankBlock[];
}

export interface TemplateDecl extends ASTNode {
    type: 'TemplateDecl';
    params: string[];
    inner: FormDecl | ImplDecl;
}

export interface ImportDecl extends ASTNode {
    type: 'ImportDecl';
    alias: string;
    path: string;
}

export interface RequiresDecl extends ASTNode {
    type: 'RequiresDecl';
    permission: string;
}

/**
 * 外部 C 符号声明（FFI）：extern <ret> <name>(<params>);
 * 声明一个由原生库（dlopen 加载）提供的外部函数，rvdl 代码可直接调用。
 * 用于把「库」（如 SteamSDK_qk）解耦为独立项目 + 原生动态库。
 */
export interface ExternDecl extends ASTNode {
    type: 'ExternDecl';
    returnType: string;
    name: string;
    params: Param[];
}

export interface FunctionDeclaration extends ASTNode {
    type: 'FunctionDeclaration';
    returnType: string;
    name: string;
    params: Param[];
    receiver: string | null;
    isPub: boolean;
    isExport: boolean;
    requires: Expression[];
    ensures: Expression[];
    body: Statement[];
    /** 函数属性（系统级）：如 @[section(".text.boot")]、@[naked] */
    attributes?: FunctionAttribute[];
    /** 运行层标签：@layer(time, thread, coord[, cost][, deadline]) —— 多维拓扑调度标签 */
    layer?: LayerTag;
    /** 量子门属性：@[gate]/@[undo]/@[steer]/@[unitary]/@[measure] */
    quantum?: QuantumAttrs;
    /** 量子物理特性：@[coherence]/@[noise]/@[basis]/@[decoherence_free]/@[error_correction] */
    physical?: PhysicalAttrs;
    /** 编译器合成函数（如 <name>_undo / <name>_steer）：不参与拓扑入口，无需 @layer */
    synthetic?: boolean;
}

/** 函数属性：@[name]、@[name("value")]、@[name(n1, n2, ...)] */
export interface FunctionAttribute {
    name: string;
    value: string | null;
    /** 数值参数列表（如 @[coherence(100, 50)] → nums=[100, 50]） */
    nums?: number[];
}

/**
 * 量子物理特性（标注在函数上一行，为 QVM/QM 物理模拟提供约束元数据）：
 * - `coherence`：@[coherence(t1, t2)] —— 相干时间（T1 弛豫 / T2 退相，单位 μs）
 * - `noise`：@[noise("depolarizing")] —— 噪声模型
 * - `basis`：@[basis(X)] —— 指定测量基（X/Y/Z）
 * - `decoherenceFree`：@[decoherence_free] —— 无退相干子空间（DFS）
 * - `errorCorrection`：@[error_correction("surface")] —— 纠错码
 */
export interface PhysicalAttrs {
    coherence?: { t1: number; t2: number };
    noise?: string;
    basis?: 'X' | 'Y' | 'Z';
    decoherenceFree?: boolean;
    errorCorrection?: string;
}

/**
 * 量子门属性（标注在函数上一行，驱动量子语义分析与门合成）。
 *
 * 命名遵循「可逆编织（Reversible Weaving）」范式:
 * - `isGate`：@[gate] —— 标记为可组合自定义门（函数体只施加门，不测量/释放）
 * - `undo`：@[undo] —— 合成可逆对偶 U†（门序反转 + 逐门取逆）。类似 uncomputation
 *   （Unqomp / Qrisp / Modular Synthesis of Efficient Quantum Uncomputation）的「块级撤销」。
 * - `steer`：@[steer] —— 合成相干控制版本 Λ(U)（控制位叠加导引目标门）。
 * 类似coherent control（相干控制）与 ZX-calculus 的图式受控视角，而非「加一个控制位」。
 * - `unitary`：@[unitary] —— 酉性验证（保证可逆：无测量、无经典分支依赖）
 * - `measure`：@[measure] —— 标记测量点（消费 Qubit，纳入 QLT 线性消费）
 */
export interface QuantumAttrs {
    isGate: boolean;
    undo: boolean;
    steer: boolean;
    unitary: boolean;
    measure: boolean;
}

/**
 * 「穿梭」策略（shuttle_strategy）。
 *
 * 五种策略的深度/代价权衡见 docs/rvdl-shuttle-tracking.md §3.4。
 * 基线对照：现有 stack 链为深度 O(N)；穿梭把深度压到 O(log N)（蝶式/树式）、
 * O(1)（Grover 扩散 / c001 蝶点）。Grover 与 c001 都是 O(1)，但资源模型相反：
 * Grover 需要 2^n 工作寄存器（缺失时降级为 BUTTERFLY 并报 W-TRK001），
 * c001 蝶点零附加资源（全体关联层只上传主要特征摘要，经蝶点中转）。
 */
export const ShuttleStrategy = {
    /** auto：运行时启发式选择（docs §3.5），选择结果写入运行期诊断，不静默决定 */
    Auto: -1,
    /** 蝶式：coord 即超立方体地址，每轮翻转一位 → 全体并行置换。深度 O(log N) */
    Butterfly: 0,
    /** 环式：最近邻。深度 O(N)，但带宽项与蝶式相同 → 大消息下可能反超蝶式 */
    Ring: 1,
    /** 树式：树汇聚/散播。深度 O(log N)，层级集群下优于蝶式 */
    Tree: 2,
    /** Grover 扩散：H^⊗n·S₀·H^⊗n 一步全局混合。深度 O(1)，需 ancilla 工作寄存器 */
    Grover: 3,
    /**
     * c001 蝶点：把「逐位翻转跳过去」改为「压缩成点再辐射」——
     * 全体关联层各上传主要特征摘要汇入蝶点，再由蝶点按相位向全体辐射。
     * 深度恒 1 轮（与 span / 对端数无关），零附加资源。docs §3.7
     */
    C001Point: 4,
} as const;
export type ShuttleStrategyValue = (typeof ShuttleStrategy)[keyof typeof ShuttleStrategy];

/**
 * 「穿梭」子标签：`shuttle=(c₀,…,c_{m−1})` + 可选调参。
 *
 * 语义：层 `a` 声明本子标签后，编译器求出
 *   𝕊(a) = { b : coord(b) = c  且 𝒢 上 a ↔ b 存在关联边 }
 * （𝒢 的边即既有五类拓扑边：stack/parallel/projection/inversion/crossing），
 * 然后以关联度 w(a,b) = exp(−d(coord_a,coord_b)/lambda) 为权重，
 * 把 a 的数据在共享点的前后双向渗透进 𝕊(a)。
 *
 * **「哪怕关联度极小也有效」的严格保证**：指数核处处严格为正，
 * 编译期与运行期**均不设截断阈值**（与注意力 softmax 丢弃小权重根本不同）。
 */
export interface ShuttleTag {
    /** 目标 coord 列表；每一项是一个独立的穿梭群目标 */
    targets: number[][];
    /** 关联度衰减长度 lambda（FS 测地 / L1 编码距离单位），必须严格 > 0 */
    lambda?: number;
    /**
     * 渗透相对共享点的相位：
     *  0 = 仅共享前（在目标层 time-slot 之前执行）
     *  1 = 仅共享后（在目标层 time-slot 之后执行）
     *  2 = 前后都渗透（默认）—— 两个传播锥的并集，每个关联层同时是源与汇，
     *      即全息编码中的「双侧恢复算子」，覆盖面严格大于 0 与 1
     */
    phase?: number;
    /** 轮次：0 = auto（= ceil(log2(span))），或显式轮数 */
    rounds?: number;
    /** 策略，见 ShuttleStrategy */
    strategy?: ShuttleStrategyValue;
}

/**
 * 「追踪」子标签：`track=(obs₀,…,obs_{k−1})` + 可选调参。
 *
 * 四段式协议（严格对应「暂时转换经典值并复制它们，将之前的值恢复到量子态」）：
 *   作用域建立（进入块的 [time, time+cost) 功能周期）
 *     ① EXTRACT  施加抽取电路（基旋转 → 测量 / QND 读出 / ancilla 计算）
 *                 量子态被暂时投影/降维 → 经典比特 b
 *     ② COPY     b 写入 slots 个经典影子槽（经典数据可任意复制，不触碰不可克隆定理）
 *     ③ RESTORE  反向运行 ① 的电路（uncompute ancilla / 按 b 重制备）
 *                 原始量子态回到线上；以 qstate_fidelity 校验
 *     作用域销毁（周期结束，影子槽随帧释放）
 *
 * **单次软克隆在物理上不可能**：测 Z 基得 b 后测量后态为 |b⟩，逆推只能得回 |0⟩，
 * 相位信息不可恢复。故保真度采用三级阶梯（见 TrackingMode），不做统一精确承诺。
 *
 * **类型约束（保 QLT 不变量）**：影子槽类型为经典标量（float/int32/double），
 * 绝非 QObject——否则直接违反 borrow.ts 的 E-Q001(no-cloning) / E-Q002(linear leak)。
 * 这与子系统码「规范量子比特被测量、逻辑量子比特存活」同构。
 */
export interface TrackTag {
    /** 要软克隆的可观测量标识符（expect_z / measure_x / entropy / …） */
    observables: string[];
    /** 保真级别，见 TrackingMode */
    mode?: number;
    /** 经典影子槽位数（复制份数），≥ 1 */
    slots?: number;
}

/**
 * 「追踪」保真阶梯。
 *
 * L0 (qnd)     Pauli 族可观测量 → 保真度 = 1 精确、零扰动、可无限次（Peierls 定理）
 * L1 (shadow)  任意可观测量   → 保真度 →1，按 O(3^(k/2)/ε) 次采样（经典阴影，
 *                               Huang-Kueng-Preskill, Nature Physics 16, 1050, 2020）
 * L2 (relabel) 已是经典值     → 保真度 = 1 精确、O(1) 自由（经典可复制）
 */
export const TrackingMode = {
    /** auto：按可观测量所属族自动选级（Pauli→L0，经典→L2，其余→L1） */
    Auto: 0,
    /** 强制 L0 qnd：非 Pauli 可观测量报 E-TRK003 */
    Qnd: 1,
    /** 强制 L1 shadow */
    Shadow: 2,
    /** 强制 L2 relabel */
    Relabel: 3,
} as const;
export type TrackingModeValue = (typeof TrackingMode)[keyof typeof TrackingMode];

/**
 * 运行层标签：@layer(time=T, thread=W, coord=(x,y,...)[, cost=N][, deadline=D][, dual=1]
 *               [, shuttle=(c,…)[, shuttle_lambda][, shuttle_phase][, shuttle_rounds][, shuttle_strategy]]
 *               [, track=(obs,…)[, track_mode][, track_slots]])
 *
 * 三维维度语义：
 * - `time`：锚点时间（块在其 coord 时序链上的启动槽位；子函数可省略，运行时继承调用者时钟 + Δt）
 * - `thread`：逻辑线程 id（同线程串行、异线程可并行）
 * - `coord`：N 维运行层坐标（块在多维拓扑空间的初始位置）
 * - `cost`：块自身的执行成本（默认 1，影响父块逻辑时钟推进）
 * - `deadline`：时间束缚上限（传播延迟超限 → 时序违例）
 *
 * 穿梭/追踪为**可选且默认惰性**的新键：缺省时既有语义与诊断逐字节不变。
 */
export interface LayerTag {
    time?: number;
    thread: number;
    coord: number[];
    cost?: number;
    deadline?: number;
    /** 对偶标记：true 表示该块处于 T-对偶（反转）空间，与同 coord 的非对偶块构成 inversion 边 */
    dual?: boolean;
    /** 穿梭子标签：把本块数据渗透进多个关联拓扑层。缺省 = 不穿梭 */
    shuttle?: ShuttleTag;
    /** 追踪子标签：软克隆可观测量到块级经典影子槽。缺省 = 不追踪 */
    track?: TrackTag;
    /**
     * 未知键记录（parseLayerTag 填充），由 semantic.ts 转成 E-TOP008。
     *
     * 既有行为是 `else if` 链无 else 分支 → 未知键被静默丢弃。
     * 穿梭/追踪引入 8 个新键后，拼写错误会让用户看不到任何提示，故改为显式记录。
     */
    unknownKeys?: { key: string; line: number; column: number }[];
}

/**
 * 拓扑边：曲面体上块之间的几何关系。
 *  - stack      叠加（同 coord，按 time 时序链）
 *  - parallel   平行（不同 coord，同维度，平行传输）
 *  - projection 投影（coord 维度不同，低维 ↔ 高维，指数/对数映射）
 *  - inversion  反转（同 coord，dual 标记不同，T-对偶 R ↔ 1/R）
 *  - crossing   交叉（同 coord，thread 不同，测地线跨线程交叉）
 *  - happens-before 时序（逻辑时钟）
 */
export interface TopologyEdge {
    from: string;
    to: string;
    kind: 'stack' | 'parallel' | 'projection' | 'inversion' | 'crossing' | 'happens-before';
}

/** 调用传播边：子函数相对父函数锚点的延迟 Δt */
export interface TopologyCallEdge {
    caller: string;
    callee: string;
    /** 子函数启动时刻（= 父函数锚点 + 前置延迟 Δt） */
    startAt: number;
    /** 相对父块锚点的延迟 */
    deltaT: number;
}

/**
 * 穿梭对端（已解析的关联层）。
 *
 * `weight` 与 `logWeight` 是同一关联度的两种表示，**必须同时提供**：
 *
 * - `logWeight = −d_L1 / lambda`：**对任意距离都不上溢/下溢**，用于运行期路由、
 *   排序与「对端是否仍可达」的判定。**这是「哪怕关联度极小也有效」的载体**——
 *   无论 d 多大，对端都保留在计划中，不会因浮点下溢而被静默丢弃。
 * - `weight = exp(logWeight)`：用于线性混合。当 d/λ ≳ 746 时双精度下溢为 0，
 *   此时该对端的混合贡献为数值零（关联低于机器精度，本就不可观测），
 *   但它**仍会被调度**，只是不贡献振幅。
 *
 * 只发 `weight` 而不发 `logWeight` 会让「极远对端」与「被阈值截断」变得不可区分，
 * 正是设计要避免的。
 */
export interface ShuttlePeer {
    /** 对端块名 */
    name: string;
    /** 对端 coord（用于运行期 FS 测地距离细化） */
    coord: number[];
    /** 对数关联度 = −d_L1 / lambda，任意距离均有限（路由/排序用） */
    logWeight: number;
    /** 线性关联度 = exp(logWeight)，混合用；d/λ ≳ 746 时下溢为 0 */
    weight: number;
    /** 编译期 L1 编码距离 */
    distance: number;
    /** 确立该关联的拓扑边种类（五类之一） */
    via: TopologyEdge['kind'];
}

/**
 * 穿梭链接：源块 → 多个已解析的关联对端。
 *
 * 这是 buildTopology 的语义层聚合产物，供 ir.ts 序列化进调度表。
 */
export interface ShuttleLink {
    /** 源块名 */
    from: string;
    /** 已解析的对端（按块名排序，保证确定性） */
    peers: ShuttlePeer[];
    /** 关联度衰减长度 lambda */
    lambda: number;
    /** 相位：0=仅前 / 1=仅后 / 2=前后（默认） */
    phase: number;
    /** 规划轮次（0 = auto，已解析为具体值） */
    rounds: number;
    /** 策略（-1 = auto，运行时启发式选择） */
    strategy: number;
    /** 每个相位（0/1）各需追加的时间槽数（0/1/2 均可为 0） */
    slotsPerPhase: [number, number];
}

/**
 * 追踪作用域：单个块的软克隆影子登记。
 *
 * `level[i]` 是 `observables[i]` 在 auto 模式下**实际选中**的保真级别
 * （0=L0 qnd / 1=L1 shadow / 2=L2 relabel），供运行期核对与诊断使用。
 */
export interface TrackingScope {
    /** 所属块名 */
    block: string;
    /** 可观测量标识符 */
    observables: string[];
    /** 请求的模式（0 = auto） */
    mode: number;
    /** 经典影子槽位数 */
    slots: number;
    /** 与 observables 一一对应的实际保真级别 */
    level: number[];
    /** 作用域窗口 [time, time+cost)，即标签所附功能周期 */
    window: [number, number];
}

/** 程序级执行拓扑（语义层聚合产物） */
export interface Topology {
    /** 运行形状：(time, thread, coord[0..n-1]) */
    shape: { time: number; thread: number; coord: number[] };
    blocks: { name: string; layer: LayerTag }[];
    edges: TopologyEdge[];
    callGraph: TopologyCallEdge[];
    /** 穿梭链接（缺省为空数组：既有程序无穿梭） */
    shuttleLinks: ShuttleLink[];
    /** 追踪作用域（缺省为空数组：既有程序无追踪） */
    trackingScopes: TrackingScope[];
}

export interface ReturnStatement extends ASTNode {
    type: 'ReturnStatement';
    argument: Expression;
    /** void 函数空返回 return; */
    isVoid?: boolean;
}

/**
 * 并发线程（Q-Digest）：spawn { ... } 派生一个并发执行单元。
 * 块体闭包继承外层作用域（共享 cap 指针 / 量子比特），是竞争检测的"线程"。
 * 命名源自量子并行（quantum parallelism）与调度中的任务派生。
 */
export interface SpawnStatement extends ASTNode {
    type: 'SpawnStatement';
    body: Statement[];
}

/**
 * 量子纠缠声明（Q-Digest）：entangle(q1, q2) 声明两比特纠缠。
 * 纠缠具有传递性（q1~q2~q3 => q1~q3），竞争检测据此构建纠缠闭包，
 * 判定跨线程的破坏性操作（测量/释放）是否触及同一闭包。
 */
export interface EntangleStatement extends ASTNode {
    type: 'EntangleStatement';
    left: Expression;
    right: Expression;
}

/**
 * 裸汇编块：asm { "..." "..." }
 * 逃逸到目标机器的原生指令序列（多指令 / 多寄存器），是 @[naked] 裸函数
 * 与中断存根 / 上下文切换这类"完整汇编体"的载体。
 * 模板为拼接后的原始汇编字符串，不做 ${} 占位符替换（区别于 native()）。
 */
export interface AsmStatement extends ASTNode {
    type: 'AsmStatement';
    template: string;
}