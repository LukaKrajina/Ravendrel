import type {
    Program,
    Statement,
    Expression,
} from './ast';

// ============================================================================
// 索引类型
// ============================================================================

/** 局部变量槽下标 */
export type LocalId = number;
/** 基本块下标 */
export type BlockId = number;

// ============================================================================
// 位置表达式（借用检查的操作对象）
// ============================================================================

/**
 * Place —— 一个可借用的内存位置。
 * `local` 为根局部变量，其后可跟若干投影。
 */
export interface MirPlace {
    local: LocalId;
    projection: MirProjection[];
}

export type MirProjection =
    | { kind: 'Field'; name: string }
    | { kind: 'Deref' }
    | { kind: 'Index'; index: LocalId };

/** 操作数是常量或位置 */
export type MirOperand =
    | { kind: 'Copy'; place: MirPlace }
    | { kind: 'Move'; place: MirPlace }
    /** 借用：`&x` / `&mut x`。量子门实参用它，表示"施加门"而非"消费量子比特"。 */
    | { kind: 'Ref'; place: MirPlace; mut: boolean }
    | { kind: 'Const'; value: MirConst };

export type MirConst =
    | { kind: 'Int'; value: number; ty: string }
    | { kind: 'Float'; value: number; ty: string }
    | { kind: 'Str'; value: string }
    | { kind: 'Char'; value: string }
    | { kind: 'Bool'; value: boolean };

// ============================================================================
// 右值（Rvalue）
// ============================================================================

export type MirRvalue =
    | { kind: 'Use'; operand: MirOperand }
    /** 借用：`&x` / `&mut x` —— 借用检查的核心产生式 */
    | { kind: 'Ref'; place: MirPlace; mut: boolean; ty: string }
    | { kind: 'Binary'; op: string; lhs: MirOperand; rhs: MirOperand; ty: string }
    | { kind: 'Unary'; op: string; arg: MirOperand; ty: string }
    | { kind: 'Call'; target: string; args: MirOperand[]; retTy: string | null }
    | { kind: 'NewObject'; className: string; args: MirOperand[]; ty: string }
    | { kind: 'Member'; object: MirOperand; property: string; ty: string; fieldIndex?: number; fieldType?: string }
    /** 裸汇编块（多指令序列），下沉路径用 InlineAsm 发射 */
    | { kind: 'Asm'; template: string }
    /** 数组字面量 [e0, e1, ...] */
    | { kind: 'Array'; elements: MirOperand[]; elemTy: string }
    /** 定长数组索引 a[i]（elemTy 为元素类型，arrTy 为完整 arr<T,N> 字符串） */
    | { kind: 'ArrayIndex'; object: MirOperand; index: MirOperand; elemTy: string; arrTy: string }
    /** 量子：分配一个 Qubit */
    | { kind: 'QAlloc'; ty: string }
    /** 量子：施加门（借用 &mut Qubit） */
    | { kind: 'QGate'; gate: string; args: MirOperand[] }
    /** 量子：测量（消费 Qubit） */
    | { kind: 'QMeasure'; arg: MirOperand; ty: string };

// ============================================================================
// 语句与终结符
// ============================================================================

export type MirStatement =
    /** place = rvalue */
    | { kind: 'Assign'; place: MirPlace; rvalue: MirRvalue }
    /** 显式释放（用于 drop 顺序与线性类型的消费点标记） */
    | { kind: 'Drop'; place: MirPlace }
    /** 线性类型的消费点（measure / release） */
    | { kind: 'Consume'; place: MirPlace };

export type MirTerminator =
    | { kind: 'Goto'; target: BlockId }
    | { kind: 'SwitchInt'; discriminant: MirOperand; targets: Array<{ value: number | null; target: BlockId }> }
    | { kind: 'Return'; value: MirOperand | null }
    | { kind: 'Call'; target: string; args: MirOperand[]; destination: MirPlace | null; next: BlockId }
    | { kind: 'Unreachable' };

// ============================================================================
// 基本块与函数体
// ============================================================================

export interface MirBasicBlock {
    id: BlockId;
    statements: MirStatement[];
    terminator: MirTerminator | null;
}

export interface MirLocalDecl {
    id: LocalId;
    /** 源变量名；临时值为 null */
    name: string | null;
    ty: string;
    mut: boolean;
    /**
     * 线性类型标记（Qubit / QObject）。
     * 线性值不可复制，必须在每条执行路径上恰好消费一次。
     */
    linear: boolean;
    /** 是否为编译器生成的临时值 */
    temporary: boolean;
    line: number;
    column: number;
}

export interface MirBody {
    /** 所属函数名（顶层脚本为 'rvdl_main'） */
    owner: string;
    locals: MirLocalDecl[];
    blocks: MirBasicBlock[];
    /** 参数对应的局部变量（按顺序） */
    params: LocalId[];
    returnTy: string | null;
    /** @layer 拓扑标签（time/thread/coord + 穿梭/追踪），下沉路径据此生成调度表与拓扑入口 */
    layer?: {
        time?: number;
        thread: number;
        coord: number[];
        cost?: number;
        deadline?: number;
        dual?: boolean;
        shuttle?: {
            targets: number[][];
            lambda?: number;
            phase?: number;
            rounds?: number;
            strategy?: number;
        };
        track?: {
            observables: string[];
            mode?: number;
            slots?: number;
        };
    };
}

export interface MirProgram {
    bodies: MirBody[];
    /** form 定义（名 → 字段列表），下沉路径据此建立结构类型（getelementptr） */
    forms?: { name: string; fields: { name: string; ty: string }[]; packed?: boolean }[];
}

// ============================================================================
// 构建器：AST -> MIR
// ============================================================================

import {
    FunctionDeclaration,
    Param,
} from './ast';

/**
 * 判断节点是否为顶层 Item（模块/类型/契约声明等）。
 * 与 ir.ts / semantic.ts 中的局部实现保持一致——它们都不是从 ast.ts 导出的。
 */
function isTopLevelItem(node: any): boolean {
    if (node.type === 'VariableDeclarationList') {
        // 顶层「显式类型」多定义 → 全局变量列表；let/auto 多定义 → 脚本模式局部变量。
        return node.varType !== 'auto' && node.varType !== 'let';
    }
    return ['ModuleDecl', 'UseDecl', 'FormDecl', 'FlavorDecl', 'ImplDecl', 'TraitDecl',
            'TemplateDecl', 'ImportDecl', 'RequiresDecl', 'ExternDecl', 'GlobalVarDecl'].includes(node.type);
}

/**
 * 线性类型：受 no-cloning 约束。
 * 这类值不可复制，必须在每条执行路径上恰好被消费一次，
 * 由 borrow.ts 的 linearity pass 检查（量子线性类型 QLT）。
 */
const LINEAR_TYPES = new Set<string>([
    'Qubit', 'QObject',
    // 具体量子类型：受 no-cloning 约束，纳入 QLT 追踪（此前漏检）。
    // 它们在 C++ 侧析构时自动 measure+release_qubit，函数末尾未消费时由
    // borrow.ts 隐式 Drop（AUTO_DROP_TYPES），而非报 E-Q002 泄漏。
    'QuantumRegister', 'DiracState', 'BellState',
]);

/** 量子门：在语句位置由 parser 识别为普通函数调用，此处还原为 QGate */
const GATE_FNS = new Set<string>([
    'x', 'h', 'y', 'z', 'rz', 'rx', 'ry', 's', 't',
    'cnot', 'toffoli', 'swap', 'qft', 'iqft', 'braid',
    // 受控门（可逆编织 @[steer] 生成）：同样借用参数（控制位 + 目标位），不消费
    'cx', 'ch', 'crz', 'cswap', 'c_toffoli', 'cqft', 'cbraid',
]);

/** 测量：消费 Qubit */
const MEASURE_FNS = new Set<string>(['measure', 'measure_x', 'measure_y']);

/**
 * QObject 借用函数（TQNF 量子态原语）：借用 QObject 实参（Ref），不消费它。
 *   qgate_*   —— QObject 层量子门（施加门，不消费）
 *   qexpect_z —— 非破坏期望读取（软测量，不消费）
 *   qattention—— SWAP test 态重叠（借用两个态，不消费）
 * 与 GATE_FNS（Qubit 门）同理，避免 QLT 把「读 QObject」误判为「消费 QObject」。
 */
const QOBJ_BORROW_FNS = new Set<string>([
    'qgate_h', 'qgate_x', 'qgate_rz', 'qgate_cnot',
    'qexpect_z', 'qattention',
    'qobj_num_qubits',
    'qgate_cnot_pair',
    'geodesic_distance',
    // QLM 推理：qlm_forward **借用** input（读写 hardware_ids/qlm_data，但不释放），
    // 因为 input 随后还要被 rvdl_decode_string 使用。而 rvdl_decode_string 是
    // 最后一次用 input，应**消费**（move）——若二者都借用，input 在函数末尾
    // 仍存活，触发 E-Q002（never consumed）；若二者都 move，则 E-Q001（两次消费）。
    // 正确语义：forward 借用 + decode 消费 = input 恰好被消费一次。
    'qlm_forward',
]);

/**
 * QObject 测量函数：测量 QObject 的第 index 个 qubit（部分坍缩），返回 int32，
 * QObject 其余 qubit 仍存活。QObject 实参用 Ref 借用（不消费整体），而非 Copy
 * （move 消费），否则 QLT 会把「测一个 qubit」误判为「消费整个 QObject」，
 * 导致同一 QObject 测多个 qubit / 测后 move 返回触发 E-Q001。
 * 与 C++ 侧 rvdl_qmeasure 的「部分坍缩」语义对齐（见 qml/QGateAbi.hpp）。
 */
const QOBJ_MEASURE_FNS = new Set<string>(['qmeasure']);

/**
 * 内建函数返回类型表（代码生成用 MIR 需要精确 retTy，供下沉到 LLVM 时映射）。
 * 与 semantic.ts 的内建签名、ir.ts 的 declare 保持一致。
 */
const BUILTIN_RET_TY: Record<string, string> = {
    // 量子核心
    'basis_state': 'QObject',
    'encode_text': 'QObject', 'encode_image': 'QObject',
    'encode_amplitudes': 'QObject',
    'encode_adaptive': 'QObject',
    'qobj_num_qubits': 'int32',
    'qgate_cnot_pair': 'void',
    'rvdl_encode_string': 'QObject', 'rvdl_decode_string': 'string',
    'qlm_invoke': 'QModel', 'qlm_load': 'QModel', 'qlm_forward': 'void',
    // 脑机接口
    'mind_read': 'QObject', 'mind_train': 'void', 'mind_feedback': 'void',
    'veda_qlm_train': 'void',
    // 神经 / 软逻辑原语
    'surrogate': 'double', 'tanh_quantize': 'double', 'lif_step': 'double',
    'mellowmax2': 'double', 'logsumexp2': 'double', 'boltzmann2': 'double',
    'tnorm_luk': 'double', 'tnorm_prod': 'double', 'tnorm_godel': 'double',
    'polymer_weight': 'double', 'polymer_mix_bound': 'double',
    // 量子时间缩微调度（tau 缩放）
    'tau_alpha': 'double', 'tau_generational': 'double', 'tau_fold_depth': 'double',
    'tau_gear_ratio': 'double', 'tau_route_cost': 'double', 'tau_route_price': 'double',
    // QCOS 系统调用
    'rvdl_sys_call': 'int32', 'rvdl_sys_calld': 'double',
    'rvdl_sys_log': 'void', 'rvdl_sys_logi': 'int32', 'rvdl_sys_callp': 'cap<uint8>',
    'rvdl_gc_alloc': 'cap<int32>', 'rvdl_gc_free': 'void',
    // QMS 数值内核
    'rvdl_qms_gap': 'double', 'rvdl_mix_bound': 'double', 'rvdl_qms_conc': 'double',
    // 系统级原子 / 端口 / 地址 / 锁
    'sync_load': 'int32', 'sync_add': 'int32', 'sync_cas': 'int32',
    'sync_store': 'void', 'sync_lock': 'int32', 'sync_unlock': 'void',
    'outb': 'void', 'inb': 'int32', 'outw': 'void', 'inw': 'int32',
    'outl': 'void', 'inl': 'int32', 'addr': 'int32',
    // 读寄存器 / MSR / CPUID（x86 架构边界）
    'read_cr0': 'uint64', 'read_cr2': 'uint64', 'read_cr3': 'uint64', 'read_cr4': 'uint64',
    'write_cr0': 'void', 'write_cr3': 'void', 'invlpg': 'void',
    'rdmsr': 'uint64', 'wrmsr': 'void', 'cpuid': 'void',
    'rdtsc': 'uint64', 'read_rflags': 'uint64', 'xgetbv': 'uint64',
    // volatile 内存访问
    'volatile_load': 'int32', 'volatile_store': 'void',
    // 复数（量子态矢量）
    'complex': 'complex128', 'real': 'double', 'imag': 'double',
    'cabs': 'double', 'conj': 'complex128',
    // QChain 量子区块链
    'qchain_wallet': 'string', 'qchain_mint': 'void', 'qchain_transfer': 'int32',
    'qchain_balance': 'uint64', 'qchain_mine': 'int32', 'qchain_height': 'int32',
    'qchain_verify': 'int32', 'qchain_qkd': 'string', 'qchain_qdba': 'int32',
    'qchain_coin_mint': 'QObject', 'qchain_coin_verify': 'int32',
    'qchain_sha3': 'string', 'qchain_hmac': 'string', 'qchain_hash_unicode': 'string',
    'qchain_sign': 'string', 'qchain_sign_verify': 'int32', 'qchain_sign_pubkey': 'string',
    'qchain_mlkem_encaps': 'string', 'qchain_mlkem_decaps': 'string',
    'qchain_causal_verify': 'int32',
    'qchain_cipher_encrypt': 'string', 'qchain_cipher_decrypt': 'string',
    // QRC 量子储备池
    'qrc_new': 'QReservoir', 'qrc_train': 'void', 'qrc_train_ex': 'void', 'qrc_release': 'void',
    'qrc_probe': 'QObject', 'qrc_predict': 'QObject',
    // TQNF 拓扑量子神经场
    'dla_dim': 'int32',
    'qstate_entropy': 'double', 'qstate_fidelity': 'double',
    // nqcmath / expermath 语言层桥接（QMathAbi.hpp）
    'qstate_purity': 'double', 'qstate_linear_entropy': 'double',
    'qstate_renyi_entropy': 'double',
    'math_dot': 'double', 'math_norm': 'double',
    'qattention': 'double',
    'shannon4': 'double', 'shannon8': 'double',
    'qgate_h': 'void', 'qgate_x': 'void', 'qgate_rz': 'void', 'qgate_cnot': 'void',
    'qexpect_z': 'double',
    'qmeasure': 'int32',
    // 量子通道逆因果容量（回程能力）
    'retrocausal_imax': 'double', 'retrocausal_idoe': 'double',
    'retrocausal_q_capacity': 'double', 'retrocausal_c_capacity': 'double',
    'retrocausal_q_one_shot': 'double',
    'retrocausal_gain': 'double', 'retrocausal_deformed': 'double',
    // 快子 KK 双空间嘈杂 CTC 通信（n 个 DD 维度）
    'retrocausal_ctc_q_capacity': 'double', 'retrocausal_ctc_c_capacity': 'double',
    'retrocausal_ctc_gain': 'double', 'retrocausal_ctc_dephasing': 'double',
    // 量子非欧几里德曲面体几何原语
    'geodesic_distance': 'double', 'inversion': 'double',
    'hyperbolic_metric': 'double', 'hyperbolic_distance': 'double',
    // 经典 GUI / 图形引擎
    'cgui_init': 'int32', 'cgui_should_close': 'int32', 'cgui_button': 'int32',
    'cgui_mouse_x': 'int32', 'cgui_mouse_y': 'int32',
    'cgui_mouse_left_clicked': 'int32', 'cgui_width': 'int32', 'cgui_height': 'int32',
    'cgui_begin_frame': 'void', 'cgui_end_frame': 'void', 'cgui_text': 'void',
    'cgui_text_int': 'void', 'cgui_beep': 'void', 'cgui_panel': 'void',
    'cgui_panel_end': 'void', 'cgui_row': 'void',
    'cgfx_rect': 'void', 'cgfx_line': 'void', 'cgfx_polyline': 'void',
    'cgfx_ellipse': 'void',
    'cgfx_triangle': 'void', 'cgfx_rect_a': 'void', 'cgfx_line_a': 'void',
    'cgfx_polyline_a': 'void',
    'cgfx_ellipse_a': 'void', 'cgfx_triangle_a': 'void',
    // 晶格内省
    'lattice_rank': 'int32', 'lattice_size': 'int32', 'lattice_boundary': 'int32',
    // 晶格访问（代码生成精确内建名，对应 ir.ts 的 rvdl_lattice_*）
    'rvdl_lattice_ref': 'int32',
};

/** 成员方法返回类型表（QObject.measure / QModel.export 等） */
const METHOD_RET_TY: Record<string, string> = {
    'measure': 'int32',
    'export': 'void',
};

/** 从常量表达式提取整数值（RouteStatement / FuseExpression 的 SwitchInt 用） */
function constInt(expr: Expression): number {
    if (expr.type === 'NumberLiteral') return expr.value;
    if (expr.type === 'CharLiteral') return expr.value.charCodeAt(0);
    return 0;
}

export class MirBuilder {
    private locals: MirLocalDecl[] = [];
    private blocks: MirBasicBlock[] = [];
    private currentBlock: BlockId = 0;
    /** 变量名 -> 局部变量槽（与 RVDL 现有语义一致：函数内为扁平作用域） */
    private varEnv = new Map<string, LocalId>();
    private loopStack: Array<{ breakTo: BlockId; continueTo: BlockId }> = [];
    private owner: string;
    /** 用户函数返回类型（代码生成用 MIR 需要精确 retTy） */
    private userFuncRet: Map<string, string> = new Map();
    /** @[gate] 自定义量子门函数名（lowerCall 时按门识别：借用参数、落到 void 槽） */
    private userGateFns: Set<string> = new Set();
    /** @[measure] 自定义测量函数名（lowerCall 时按测量识别：消费 Qubit） */
    private userMeasureFns: Set<string> = new Set();
    /** form 定义（名 → 字段列表），Member 字段访问据此查字段索引/类型 */
    private formFields: Map<string, { name: string; ty: string }[]> = new Map();
    /** @[packed] form 名集合（无 vtable、无填充、字段从偏移 0 起） */
    private packedForms: Set<string> = new Set();
    /** flavor 成员（名 → 整数值），MIR 里内联为常量 */
    private flavorMembers: Map<string, number> = new Map();
    /** 全局变量（名 → 类型），MIR 里降为占位常量（借用检查不关心经典全局状态） */
    private globalSymbols: Set<string> = new Set();

    constructor(owner: string = 'rvdl_main') {
        this.owner = owner;
        this.blocks.push({ id: 0, statements: [], terminator: null });
    }

    // ---- 基础设施 ----------------------------------------------------------

    private newLocal(name: string | null, ty: string, mut: boolean,
                     temporary: boolean, line: number, column: number): LocalId {
        const id = this.locals.length;
        this.locals.push({
            id, name, ty, mut,
            linear: LINEAR_TYPES.has(ty),
            temporary, line, column,
        });
        return id;
    }

    private newBlock(): BlockId {
        const id = this.blocks.length;
        this.blocks.push({ id, statements: [], terminator: null });
        return id;
    }

    private place(local: LocalId): MirPlace {
        return { local, projection: [] };
    }

    private push(stmt: MirStatement): void {
        this.blocks[this.currentBlock].statements.push(stmt);
    }

    private terminate(term: MirTerminator): void {
        this.blocks[this.currentBlock].terminator = term;
    }

    private enterBlock(id: BlockId): void {
        this.currentBlock = id;
    }

    /** 把值落入一个显式临时局部变量（MIR 要求每个中间结果都可见） */
    private materialize(rvalue: MirRvalue, ty: string, line: number, column: number): MirPlace {
        const tmp = this.newLocal(null, ty, true, true, line, column);
        this.push({ kind: 'Assign', place: this.place(tmp), rvalue });
        return this.place(tmp);
    }

    private operandType(op: MirOperand): string {
        if (op.kind === 'Const') {
            switch (op.value.kind) {
                case 'Int':   return op.value.ty;
                case 'Float': return op.value.ty;
                case 'Str':   return 'string';
                case 'Char':  return 'char';
                case 'Bool':  return 'bool';
            }
        }
        return this.locals[op.place.local].ty;
    }

    /** 二元运算的结果类型（比较运算为 bool，其余沿用左操作数类型） */
    private binaryType(operator: string, lhs: MirOperand): string {
        if (operator === '<' || operator === '>' || operator === '<=' ||
            operator === '>=' || operator === '==' || operator === '!=') {
            return 'bool';
        }
        if (operator === '&&' || operator === '||') {
            return 'bool';
        }
        const t = this.operandType(lhs);
        return t === 'unknown' ? 'int32' : t;
    }

    // ---- 表达式降级 --------------------------------------------------------

    private lowerExpression(expr: Expression): MirOperand {
        switch (expr.type) {
            case 'NumberLiteral': {
                const isFloat = expr.isFloat === true;
                return {
                    kind: 'Const',
                    value: { kind: isFloat ? 'Float' : 'Int', value: expr.value, ty: isFloat ? 'double' : 'int32' },
                };
            }

            case 'StringLiteral':
                return { kind: 'Const', value: { kind: 'Str', value: expr.value } };

            case 'CharLiteral':
                return { kind: 'Const', value: { kind: 'Char', value: expr.value } };

            case 'NullLiteral':
                // null 指针 -> 值 0、类型 i8*
                return { kind: 'Const', value: { kind: 'Int', value: 0, ty: 'i8*' } };

            case 'Dereference':
                // 裸指针解引用属于 unsafe 语义；借用检查 v1 不深入追踪指针别名，
                // 降为对指针本身的一次读取（真正的 typed load 在 IR 层完成）。
                return this.lowerExpression(expr.target);

            case 'AddressOf': {
                // &x（局部变量）→ 持久借用：降为 Ref 右值落到临时，供 borrow.ts
                //   追踪 loan 活跃区间（Polonius 子集式）；&fn（函数名）→ 函数
                //   地址常量，不追踪借用。
                if (expr.target.type === 'Identifier') {
                    const id = this.varEnv.get(expr.target.name);
                    if (id !== undefined) {
                        const p = this.materialize(
                            { kind: 'Ref', place: this.place(id), mut: false, ty: 'i8*' },
                            'i8*', expr.line, expr.column);
                        return { kind: 'Copy', place: p };
                    }
                }
                return { kind: 'Const', value: { kind: 'Int', value: 0, ty: 'i8*' } };
            }

            case 'FuseExpression': {
                // fuse (x) { v1: e1, ..., _: ef } 降为 SwitchInt 控制流（代码生成用精确降级）。
                // 判别值路由到匹配 arm，求值后 Goto 汇合；通配 `_`（pattern==null）作 default。
                const result = this.newLocal(null, 'int32', true, true, expr.line, expr.column);
                const disc = this.lowerExpression(expr.discriminant);

                const armBlocks = expr.arms.map(() => this.newBlock());
                const afterBlock = this.newBlock();

                const targets = expr.arms.map((arm, i) => ({
                    value: arm.pattern ? constInt(arm.pattern) : null,
                    target: armBlocks[i],
                }));
                this.terminate({ kind: 'SwitchInt', discriminant: disc, targets });

                expr.arms.forEach((arm, i) => {
                    this.enterBlock(armBlocks[i]);
                    const v = this.lowerExpression(arm.value);
                    this.push({
                        kind: 'Assign',
                        place: this.place(result),
                        rvalue: { kind: 'Use', operand: v },
                    });
                    this.terminate({ kind: 'Goto', target: afterBlock });
                });

                this.enterBlock(afterBlock);
                return { kind: 'Copy', place: this.place(result) };
            }

            case 'NativeExpression': {
                // 原生指令是 side-effect；操作数仅做求值（借用检查覆盖），结果忽略。
                for (const op of expr.operands) this.lowerExpression(op);
                const p = this.materialize(
                    { kind: 'Call', target: 'native', args: [], retTy: 'void' },
                    'void', expr.line, expr.column);
                return { kind: 'Copy', place: p };
            }

            case 'ArrayLiteral': {
                // 数组字面量：元素依次求值，落到 Array 右值；元素类型取首个元素（或 unknown）。
                const elements = expr.elements.map(e => this.lowerExpression(e));
                const elemTy = elements.length > 0 ? this.operandType(elements[0]) : 'int32';
                const p = this.materialize(
                    { kind: 'Array', elements, elemTy },
                    `arr<${elemTy}, ${elements.length}>`, expr.line, expr.column);
                return { kind: 'Copy', place: p };
            }

            case 'Identifier': {
                // flavor 成员：内联为整型常量
                if (this.flavorMembers.has(expr.name)) {
                    const tmp = this.materialize(
                        { kind: 'Use', operand: { kind: 'Const', value: { kind: 'Int', value: this.flavorMembers.get(expr.name)!, ty: 'int32' } } },
                        'int32', expr.line, expr.column);
                    return { kind: 'Copy', place: tmp };
                }
                // 全局变量：降为占位常量（借用检查不关心经典全局状态）
                if (this.globalSymbols.has(expr.name)) {
                    const tmp = this.materialize(
                        { kind: 'Use', operand: { kind: 'Const', value: { kind: 'Int', value: 0, ty: 'int32' } } },
                        'int32', expr.line, expr.column);
                    return { kind: 'Copy', place: tmp };
                }
                const id = this.varEnv.get(expr.name);
                if (id === undefined) {
                    throw new Error(`MIR Error: unresolved variable '${expr.name}' (line ${expr.line})`);
                }
                return { kind: 'Copy', place: this.place(id) };
            }

            case 'BinaryExpression': {
                const lhs = this.lowerExpression(expr.left);
                const rhs = this.lowerExpression(expr.right);
                const ty = this.binaryType(expr.operator, lhs);
                const p = this.materialize(
                    { kind: 'Binary', op: expr.operator, lhs, rhs, ty },
                    ty, expr.line, expr.column);
                return { kind: 'Copy', place: p };
            }

            case 'UnaryExpression': {
                const arg = this.lowerExpression(expr.argument);
                const ty = expr.operator === '!' ? 'bool' : this.operandType(arg);
                const p = this.materialize(
                    { kind: 'Unary', op: expr.operator, arg, ty },
                    ty, expr.line, expr.column);
                return { kind: 'Copy', place: p };
            }

            /**
             * 短回路求值 —— 这是"为什么必须在 MIR 上做"的典型例子：
             * `a && b` 在 AST 里只是一个嵌套表达式，无法表达"b 可能不被求值"；
             * 降为 MIR 后表现为显式的条件分支，借用检查才能正确判断
             * b 中的借用是否活跃。
             */
            case 'LogicalExpression': {
                const result = this.newLocal(null, 'bool', true, true, expr.line, expr.column);
                const lhs = this.lowerExpression(expr.left);

                const shortCircuitBlock = this.newBlock();  // 需要求值右操作数的块
                const resultBlock       = this.newBlock();  // 直接取短路结果的块
                const afterBlock        = this.newBlock();

                const isAnd = expr.operator === '&&';
                this.terminate({
                    kind: 'SwitchInt',
                    discriminant: lhs,
                    targets: [
                        { value: 1,   target: isAnd ? shortCircuitBlock : resultBlock },
                        { value: null, target: isAnd ? resultBlock : shortCircuitBlock },
                    ],
                });

                // 短路结果：&& 为 false，|| 为 true
                this.enterBlock(resultBlock);
                this.push({
                    kind: 'Assign',
                    place: this.place(result),
                    rvalue: { kind: 'Use', operand: { kind: 'Const', value: { kind: 'Bool', value: !isAnd } } },
                });
                this.terminate({ kind: 'Goto', target: afterBlock });

                // 求值右操作数
                this.enterBlock(shortCircuitBlock);
                const rhs = this.lowerExpression(expr.right);
                this.push({
                    kind: 'Assign',
                    place: this.place(result),
                    rvalue: { kind: 'Use', operand: rhs },
                });
                this.terminate({ kind: 'Goto', target: afterBlock });

                this.enterBlock(afterBlock);
                return { kind: 'Copy', place: this.place(result) };
            }

            case 'FunctionCall':
                return this.lowerCall(expr.name, expr.arguments, expr.line, expr.column);

            case 'NewExpression': {
                const args = expr.arguments.map(a => this.lowerExpression(a));
                const p = this.materialize(
                    { kind: 'NewObject', className: expr.className, args, ty: expr.className },
                    expr.className, expr.line, expr.column);
                return { kind: 'Copy', place: p };
            }

            case 'MemberExpression': {
                const object = this.lowerExpression(expr.object);
                if (expr.isMethodCall) {
                    // 成员方法调用：降级为以其属性名为目标的调用，retTy 由方法签名表推导
                    const args = (expr.arguments ?? []).map(a => this.lowerExpression(a));
                    const retTy = METHOD_RET_TY[expr.property] ?? 'int32';
                    // measure() 是消费操作（消费整个 QObject：measure + release 所有 qubit）。
                    // 显式 Consume object，否则 QLT 会误判 object 未消费（E-Q002）。
                    if (expr.property === 'measure' && object.kind === 'Copy' &&
                        this.locals[object.place.local].linear) {
                        this.push({ kind: 'Consume', place: object.place });
                    }
                    const p = this.materialize(
                        { kind: 'Call', target: expr.property, args, retTy }, retTy, expr.line, expr.column);
                    return { kind: 'Copy', place: p };
                }
                // 查字段索引/类型（对象为 form 时），下沉路径据此生成 getelementptr
                let fieldIndex: number | undefined;
                let fieldType: string | undefined;
                if (expr.object.type === 'Identifier') {
                    const id = this.varEnv.get(expr.object.name);
                    if (id !== undefined && id < this.locals.length) {
                        const fields = this.formFields.get(this.locals[id].ty);
                        if (fields) {
                            const idx = fields.findIndex(f => f.name === expr.property);
                            if (idx >= 0) {
                                // packed form：无 vtable，字段从 0 起；普通 form：field 0 是 vtable 指针
                                fieldIndex = this.packedForms.has(this.locals[id].ty) ? idx : idx + 1;
                                fieldType = fields[idx].ty;
                            }
                        }
                    }
                }
                const p = this.materialize(
                    { kind: 'Member', object, property: expr.property, ty: fieldType ?? 'unknown', fieldIndex, fieldType },
                    fieldType ?? 'unknown', expr.line, expr.column);
                return { kind: 'Copy', place: p };
            }

            case 'IndexExpression': {
                // 字符串遍历：s[i] → rvdl_str_index(s, i)（char）；数组索引 → ArrayIndex；晶格索引 → rvdl_lattice_ref
                if (expr.object.type === 'Identifier') {
                    const id = this.varEnv.get(expr.object.name);
                    if (id !== undefined && id < this.locals.length) {
                        const ty = this.locals[id].ty;
                        if (ty === 'string') {
                            return this.lowerCall('rvdl_str_index', [expr.object, ...expr.indices], expr.line, expr.column);
                        }
                        if (ty.startsWith('arr<')) {
                            const elemTy = ty.slice(4, -1).split(',')[0].trim();
                            // object 用 Ref（取数组 alloca 地址），供下沉路径 GEP + load
                            const obj: MirOperand = { kind: 'Ref', place: this.place(id), mut: false };
                            const index = this.lowerExpression(expr.indices[0]);
                            const p = this.materialize(
                                { kind: 'ArrayIndex', object: obj, index, elemTy, arrTy: ty },
                                elemTy, expr.line, expr.column);
                            return { kind: 'Copy', place: p };
                        }
                    }
                }
                // 晶格索引读取：降为精确内建 rvdl_lattice_ref（对应 ir.ts 的 declare i32 @rvdl_lattice_ref）
                return this.lowerCall('rvdl_lattice_ref', [expr.object, ...expr.indices], expr.line, expr.column);
            }

            case 'FunctionExpression': {
                // lambda 表达式：内联 lower body（借用检查覆盖 lambda body 的量子操作），
                // 参数作为局部变量；返回占位闭包值（完整闭包语义由 ir.ts 层承载）。
                for (const p of expr.params) {
                    const id = this.newLocal(p.name, p.type, true, false, expr.line, expr.column);
                    this.varEnv.set(p.name, id);
                }
                for (const s of expr.body) this.lowerStatement(s);
                const tmp = this.materialize(
                    { kind: 'Use', operand: { kind: 'Const', value: { kind: 'Int', value: 0, ty: 'int32' } } },
                    'int32', expr.line, expr.column);
                return { kind: 'Copy', place: tmp };
            }

            default:
                throw new Error(`MIR Error: unsupported expression '${(expr as any).type}`);
        }
    }

    /** 把量子门的实参降为可变借用（&mut），表示施加门而非消费 */
    private lowerBorrowArg(expr: Expression): MirOperand {
        if (expr.type === 'Identifier') {
            const id = this.varEnv.get(expr.name);
            if (id === undefined) {
                throw new Error(`MIR Error: unresolved variable '${expr.name}' (line ${expr.line})`);
            }
            return { kind: 'Ref', place: this.place(id), mut: true };
        }
        // 非常量标识符的实参（如寄存器下标等）按普通表达式处理
        return this.lowerExpression(expr);
    }

    /** 函数返回类型：内建表 > 用户函数表 > 默认 int32 */
    private retTyOf(name: string): string {
        const builtin = BUILTIN_RET_TY[name];
        if (builtin !== undefined) return builtin;
        const user = this.userFuncRet.get(name);
        if (user !== undefined) return user;
        return 'int32';
    }

    /** 函数调用：区分量子门 / 测量 / 普通调用（普通调用返回类型由签名表精确推导） */
    private lowerCall(name: string, args: Expression[], line: number, column: number): MirOperand {
        if (name === 'alloc') {
            const p = this.materialize({ kind: 'QAlloc', ty: 'Qubit' }, 'Qubit', line, column);
            return { kind: 'Copy', place: p };
        }

        if (GATE_FNS.has(name) || this.userGateFns.has(name)) {
            // 门**借用**量子比特，不消费它；因此实参用 Ref 而非 Copy/Move，
            // 否则 QLT 会把"施加门"误判为"消耗量子比特"。
            const lowered = args.map(a => this.lowerBorrowArg(a));
            // 门不产生值；落到一个 void 槽以保持"每个语句只做一件事"
            const p = this.materialize({ kind: 'QGate', gate: name, args: lowered }, 'void', line, column);
            return { kind: 'Copy', place: p };
        }

        if (QOBJ_BORROW_FNS.has(name)) {
            // QObject 借用函数：QObject 实参用 Ref 借用（不消费），其他实参
            // （int32 index / double angle）经 lowerBorrowArg 落到普通表达式。
            const lowered = args.map(a => this.lowerBorrowArg(a));
            const retTy = this.retTyOf(name);
            const p = this.materialize(
                { kind: 'Call', target: name, args: lowered, retTy }, retTy, line, column);
            return { kind: 'Copy', place: p };
        }

        if (QOBJ_MEASURE_FNS.has(name)) {
            // QObject 测量（部分坍缩）：QObject 实参用 Ref 借用（不消费整体），
            // index 实参经 lowerBorrowArg 落到普通表达式。
            const lowered = args.map(a => this.lowerBorrowArg(a));
            const p = this.materialize(
                { kind: 'Call', target: name, args: lowered, retTy: 'int32' }, 'int32', line, column);
            return { kind: 'Copy', place: p };
        }

        if (MEASURE_FNS.has(name) || this.userMeasureFns.has(name)) {
            const lowered = args.map(a => this.lowerExpression(a));
            const p = this.materialize(
                { kind: 'QMeasure', arg: lowered[0], ty: 'int32' }, 'int32', line, column);
            return { kind: 'Copy', place: p };
        }

        const lowered = args.map(a => this.lowerExpression(a));
        const retTy = this.retTyOf(name);
        const p = this.materialize(
            { kind: 'Call', target: name, args: lowered, retTy }, retTy, line, column);
        return { kind: 'Copy', place: p };
    }

    // ---- 语句降级 ----------------------------------------------------------

    private lowerStatement(stmt: Statement): void {
        switch (stmt.type) {
            case 'VariableDeclarationList': {
                for (const d of stmt.declarations) this.lowerStatement(d);
                break;
            }
            case 'VariableDeclaration': {
                // &x（对局部变量的持久借用）：直接生成 `r = Ref x`，让 r 成为 loan
                //   载体，使 loan 活跃区间 = r 的 liveness（而非被立即 Copy 的中间临时）。
                if (stmt.value.type === 'AddressOf' && stmt.value.target.type === 'Identifier') {
                    const borrowed = this.varEnv.get(stmt.value.target.name);
                    if (borrowed !== undefined) {
                        const local = this.newLocal(stmt.identifier, 'i8*', true, false, stmt.line, stmt.column);
                        this.push({
                            kind: 'Assign',
                            place: this.place(local),
                            rvalue: { kind: 'Ref', place: this.place(borrowed), mut: false, ty: 'i8*' },
                        });
                        this.varEnv.set(stmt.identifier, local);
                        break;
                    }
                }
                const operand = this.lowerExpression(stmt.value);
                const ty = stmt.varType === 'auto' ? this.operandType(operand) : stmt.varType;
                const local = this.newLocal(stmt.identifier, ty, true, false, stmt.line, stmt.column);
                this.push({ kind: 'Assign', place: this.place(local), rvalue: { kind: 'Use', operand } });
                this.varEnv.set(stmt.identifier, local);
                break;
            }

            case 'ExpressionStatement': {
                this.lowerExpression(stmt.expression);
                break;
            }

            case 'AssignmentStatement': {
                // 裸指针解引用赋值：*p = v（借用检查 v1 不追踪指针别名）
                if (stmt.target && stmt.target.type === 'Dereference') {
                    this.lowerExpression(stmt.target.target);
                    this.lowerExpression(stmt.value);
                    break;
                }
                // 晶格索引赋值 board[x,y] = v / 字段赋值 obj.field = v：作为副作用降级
                if (stmt.target && (stmt.target.type === 'IndexExpression' || stmt.target.type === 'MemberExpression')) {
                    const targetOperand = this.lowerExpression(stmt.target);
                    const val = this.lowerExpression(stmt.value);
                    // 线性类型字段赋值（如 r.c = q）：
                    //   - 读出的旧字段值（Member 临时）被新值覆盖，需消费（丢弃旧值）；
                    //   - value 被 move 到字段，需消费。
                    // 否则 QLT 会把这两个 Copy 临时判为泄漏（E-Q002）。
                    if (targetOperand.kind === 'Copy' && this.locals[targetOperand.place.local].linear) {
                        this.push({ kind: 'Consume', place: targetOperand.place });
                    }
                    if (val.kind === 'Copy' && this.locals[val.place.local].linear) {
                        this.push({ kind: 'Consume', place: val.place });
                    }
                    break;
                }
                // 全局变量赋值：借用检查不追踪经典全局状态（与全局读取同理），
                // 但右值仍需降级 —— 其中的量子操作与线性类型的移动必须被检查到。
                // 否则任何含全局赋值的程序都会让整个借用检查被跳过（best-effort 降级）。
                if (this.globalSymbols.has(stmt.name)) {
                    const globalVal = this.lowerExpression(stmt.value);
                    if (globalVal.kind === 'Copy' && this.locals[globalVal.place.local].linear) {
                        this.push({ kind: 'Consume', place: globalVal.place });
                    }
                    break;
                }
                const id = this.varEnv.get(stmt.name);
                if (id === undefined) {
                    throw new Error(`MIR Error: assignment to unresolved variable '${stmt.name}' (line ${stmt.line})`);
                }
                const operand = this.lowerExpression(stmt.value);
                this.push({ kind: 'Assign', place: this.place(id), rvalue: { kind: 'Use', operand } });
                break;
            }

            case 'ReturnStatement': {
                const operand = stmt.argument ? this.lowerExpression(stmt.argument) : null;
                this.terminate({ kind: 'Return', value: operand });
                break;
            }

            case 'IfStatement': {
                const cond = this.lowerExpression(stmt.condition);
                const thenB = this.newBlock();
                const elseB = (stmt.alternate && stmt.alternate.length > 0) ? this.newBlock() : null;
                const afterB = this.newBlock();

                this.terminate({
                    kind: 'SwitchInt',
                    discriminant: cond,
                    targets: [
                        { value: 1,   target: thenB },
                        { value: null, target: elseB ?? afterB },
                    ],
                });

                this.enterBlock(thenB);
                for (const s of stmt.consequent) this.lowerStatement(s);
                this.terminate({ kind: 'Goto', target: afterB });

                if (elseB) {
                    this.enterBlock(elseB);
                    for (const s of stmt.alternate!) this.lowerStatement(s);
                    this.terminate({ kind: 'Goto', target: afterB });
                }

                this.enterBlock(afterB);
                break;
            }

            case 'UnsafeBlock': {
                for (const s of stmt.body) this.lowerStatement(s);
                break;
            }

            case 'RouteStatement': {
                const disc = this.lowerExpression(stmt.discriminant);
                const caseBlocks = stmt.cases.map(() => this.newBlock());
                const fallbackB = stmt.fallback ? this.newBlock() : null;
                const afterB = this.newBlock();
                const targets = stmt.cases.map((c, i) => ({
                    value: constInt(c.value),
                    target: caseBlocks[i],
                }));
                this.terminate({
                    kind: 'SwitchInt',
                    discriminant: disc,
                    targets: [...targets, { value: null, target: fallbackB ?? afterB }],
                });
                stmt.cases.forEach((c, i) => {
                    this.enterBlock(caseBlocks[i]);
                    for (const s of c.body) this.lowerStatement(s);
                    this.terminate({ kind: 'Goto', target: afterB });
                });
                if (fallbackB) {
                    this.enterBlock(fallbackB);
                    for (const s of stmt.fallback!) this.lowerStatement(s);
                    this.terminate({ kind: 'Goto', target: afterB });
                }
                this.enterBlock(afterB);
                break;
            }

            case 'SpinStatement': {
                const bodyB = this.newBlock();
                const condB = this.newBlock();
                const afterB = this.newBlock();

                // 先执行体
                this.terminate({ kind: 'Goto', target: bodyB });

                this.loopStack.push({ breakTo: afterB, continueTo: condB });
                this.enterBlock(bodyB);
                for (const s of stmt.body) this.lowerStatement(s);
                this.loopStack.pop();
                this.terminate({ kind: 'Goto', target: condB });

                // 再判断条件
                this.enterBlock(condB);
                const cond = this.lowerExpression(stmt.condition);
                this.terminate({
                    kind: 'SwitchInt',
                    discriminant: cond,
                    targets: [
                        { value: 1, target: bodyB },
                        { value: null, target: afterB },
                    ],
                });

                this.enterBlock(afterB);
                break;
            }

            case 'WhileStatement': {
                const condB = this.newBlock();
                const bodyB = this.newBlock();
                const elseB = (stmt.elseBody && stmt.elseBody.length > 0) ? this.newBlock() : null;
                const afterB = this.newBlock();

                this.terminate({ kind: 'Goto', target: condB });

                this.enterBlock(condB);
                const cond = this.lowerExpression(stmt.condition);
                this.terminate({
                    kind: 'SwitchInt',
                    discriminant: cond,
                    targets: [
                        { value: 1,   target: bodyB },
                        { value: null, target: elseB ?? afterB },
                    ],
                });

                this.loopStack.push({ breakTo: afterB, continueTo: condB });
                this.enterBlock(bodyB);
                for (const s of stmt.body) this.lowerStatement(s);
                this.loopStack.pop();
                this.terminate({ kind: 'Goto', target: condB });

                if (elseB) {
                    this.enterBlock(elseB);
                    for (const s of stmt.elseBody!) this.lowerStatement(s);
                    this.terminate({ kind: 'Goto', target: afterB });
                }

                this.enterBlock(afterB);
                break;
            }

            case 'ForStatement': {
                if (stmt.init) this.lowerStatement(stmt.init);

                const condB = this.newBlock();
                const bodyB = this.newBlock();
                const updateB = this.newBlock();
                const afterB = this.newBlock();

                this.terminate({ kind: 'Goto', target: condB });

                this.enterBlock(condB);
                if (stmt.condition) {
                    const cond = this.lowerExpression(stmt.condition);
                    this.terminate({
                        kind: 'SwitchInt',
                        discriminant: cond,
                        targets: [
                            { value: 1,   target: bodyB },
                            { value: null, target: afterB },
                        ],
                    });
                } else {
                    this.terminate({ kind: 'Goto', target: bodyB });
                }

                this.loopStack.push({ breakTo: afterB, continueTo: updateB });
                this.enterBlock(bodyB);
                for (const s of stmt.body) this.lowerStatement(s);
                this.loopStack.pop();
                this.terminate({ kind: 'Goto', target: updateB });

                this.enterBlock(updateB);
                if (stmt.update) this.lowerStatement(stmt.update);
                this.terminate({ kind: 'Goto', target: condB });

                this.enterBlock(afterB);
                break;
            }

            case 'BreakStatement': {
                const top = this.loopStack[this.loopStack.length - 1];
                if (!top) {
                    throw new Error(`MIR Error: 'break' outside of loop (line ${stmt.line})`);
                }
                this.terminate({ kind: 'Goto', target: top.breakTo });
                break;
            }

            case 'ContinueStatement': {
                const top = this.loopStack[this.loopStack.length - 1];
                if (!top) {
                    throw new Error(`MIR Error: 'continue' outside of loop (line ${stmt.line})`);
                }
                this.terminate({ kind: 'Goto', target: top.continueTo });
                break;
            }

            case 'FunctionDeclaration':
                // 顶层函数由 lowerProgram 处理；此处忽略（RVDL 不支持嵌套函数定义）。
                break;

            case 'SpawnStatement':
                // spawn 块体：MIR 层内联 lower（并发语义由 ir 层 + runtime 承载，
                // MIR 仅用于借用检查 / 竞争检测的输入）。
                for (const s of stmt.body) this.lowerStatement(s);
                break;

            case 'EntangleStatement':
                // 纠缠声明：纯静态信息（供竞争检测），无运行时指令。
                break;

            case 'AsmStatement': {
                // 裸汇编块：无操作数，作为 void side-effect 落到临时槽（下沉路径经 InlineAsm 发射）。
                const p = this.materialize(
                    { kind: 'Asm', template: stmt.template }, 'void', stmt.line, stmt.column);
                break;
            }

            default:
                throw new Error(`MIR Error: unsupported statement '${(stmt as any).type}'`);
        }
    }

    // ---- 函数体与程序降级 ---------------------------------------------------

    private lowerBody(owner: string, params: Param[], body: Statement[],
                      returnTy: string | null,
                      layer?: MirBody['layer']): MirBody {
        this.owner = owner;
        this.locals = [];
        this.blocks = [];
        this.varEnv = new Map();
        this.loopStack = [];
        this.currentBlock = 0;
        this.blocks.push({ id: 0, statements: [], terminator: null });

        const paramIds: LocalId[] = [];
        for (const p of params) {
            const id = this.newLocal(p.name, p.type, true, false, 0, 0);
            paramIds.push(id);
            this.varEnv.set(p.name, id);
        }

        for (const s of body) this.lowerStatement(s);

        // 末块若无终结符，补一个终结符，保证 CFG 良好。
        const last = this.blocks[this.currentBlock];
        if (last.terminator === null) {
            last.terminator = (returnTy && returnTy !== 'void')
                ? { kind: 'Unreachable' }
                : { kind: 'Return', value: null };
        }

        return { owner, locals: this.locals, blocks: this.blocks, params: paramIds, returnTy, ...(layer ? { layer } : {}) };
    }

    /** 把一个顶层函数声明降为 MirBody */
    private lowerFunction(fn: FunctionDeclaration): MirBody {
        const lt = fn.layer;
        const layer = lt
            ? {
                time: lt.time,
                thread: lt.thread,
                coord: lt.coord,
                cost: lt.cost,
                deadline: lt.deadline,
                dual: lt.dual,
                shuttle: lt.shuttle
                    ? {
                        targets: lt.shuttle.targets,
                        lambda: lt.shuttle.lambda,
                        phase: lt.shuttle.phase,
                        rounds: lt.shuttle.rounds,
                        strategy: lt.shuttle.strategy,
                    }
                    : undefined,
                track: lt.track
                    ? {
                        observables: lt.track.observables,
                        mode: lt.track.mode,
                        slots: lt.track.slots,
                    }
                    : undefined,
              }
            : undefined;
        return this.lowerBody(fn.name, fn.params, fn.body, fn.returnType, layer);
    }

    /** 把整份程序降为 MirProgram */
    public build(program: Program): MirProgram {
        // 收集用户函数返回类型（代码生成用 MIR 需要精确 retTy）+ 量子门/测量函数名
        this.userFuncRet.clear();
        this.userGateFns.clear();
        this.userMeasureFns.clear();
        this.formFields.clear();
        this.packedForms.clear();
        this.flavorMembers.clear();
        this.globalSymbols.clear();
        for (const n of program.body) {
            if (n.type === 'FunctionDeclaration') {
                this.userFuncRet.set(n.name, n.returnType);
                const q = (n as FunctionDeclaration).quantum;
                if (q) {
                    if (q.isGate) this.userGateFns.add(n.name);
                    if (q.measure) this.userMeasureFns.add(n.name);
                }
            } else if (n.type === 'FormDecl') {
                // 收集 form 字段（ranks 展开），供 Member 字段访问查索引/类型
                const form = n as any;
                const fields: { name: string; ty: string }[] = [];
                for (const rank of form.ranks) {
                    for (const f of rank.fields) fields.push({ name: f.name, ty: f.type });
                }
                this.formFields.set(form.name, fields);
                if (form.packed) this.packedForms.add(form.name);
            } else if (n.type === 'FlavorDecl') {
                // 收集 flavor 成员（显式值优先，自动递增），MIR 里内联为常量
                const flavor = n as any;
                let next = 0;
                for (const m of flavor.members) {
                    const value = m.value ?? next;
                    this.flavorMembers.set(m.name, value);
                    next = value + 1;
                }
            } else if (n.type === 'GlobalVarDecl') {
                // 收集全局变量名（MIR 里降为占位常量）
                this.globalSymbols.add(n.name);
            } else if (n.type === 'VariableDeclarationList' && n.varType !== 'auto' && n.varType !== 'let') {
                for (const d of n.declarations) this.globalSymbols.add(d.identifier);
            }
        }

        const bodies: MirBody[] = [];
        const fnDecls = program.body.filter(
            (n): n is FunctionDeclaration => n.type === 'FunctionDeclaration');

        if (fnDecls.length > 0) {
            for (const fn of fnDecls) {
                bodies.push(this.lowerFunction(fn));
            }
        } else {
            const stmts = program.body.filter(
                (n): n is Statement => !isTopLevelItem(n));
            bodies.push(this.lowerBody('rvdl_main', [], stmts, 'int32'));
        }

        const forms = [...this.formFields.entries()].map(([name, fields]) => ({
            name, fields, ...(this.packedForms.has(name) ? { packed: true } : {})
        }));
        return { bodies, ...(forms.length > 0 ? { forms } : {}) };
    }
}

/** 便捷入口：AST -> MIR */
export function buildMir(program: Program): MirProgram {
    return new MirBuilder().build(program);
}