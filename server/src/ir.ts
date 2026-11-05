import { Expression, Program, Statement, FunctionDeclaration, ReturnStatement, Item, FormDecl, TraitDecl, ImplDecl, TemplateDecl, FnDecl, FieldDecl, RankBlock, Param, GlobalVarDecl, TopologyCallEdge, Topology } from './ast';
import { verifyIR } from './irverify';
import { buildTopology } from './topology';

/**
 * @[noise] 未给出强度、且也无 @[coherence] 可推导时的保守默认噪声强度。
 * 1% 是典型未标定器件的保守上界；一旦给了 @[coherence]，强度会从 T1/T2
 * 物理推导（见 defaultNoiseStrength），不再用这个固定值。
 */
const DEFAULT_NOISE_PARAM = 0.01;

/**
 * 从 @[coherence] 的 T1/T2 推导「未指定强度」噪声通道的默认强度。
 * 各通道对应真实物理退相干来源：
 *   振幅阻尼(2)   ← T1 能量弛豫：1/T1
 *   相位阻尼(1)   ← 纯退相：1/T_φ = 1/T2 − 1/(2·T1)
 *   位翻转(3)     ← 同纯退相
 *   去极化(0)     ← 总退相干速率：1/T1 + 1/T2
 */
function defaultNoiseStrength(coherence: { t1: number; t2?: number }, channel: number): number {
    const t1 = Math.max(coherence.t1, 1.0);
    const t2 = coherence.t2;
    const invTphi = t2 && t2 > 0
        ? 1.0 / Math.max(t2, 1.0) - 1.0 / (2.0 * t1)
        : 0.0;
    switch (channel) {
        case 2: return Math.min(1.0, 1.0 / t1);                       // 振幅阻尼
        case 1: return invTphi > 0 ? Math.min(1.0, invTphi) : 0.0;    // 相位阻尼
        case 3: return invTphi > 0 ? Math.min(1.0, invTphi) : 0.0;    // 位翻转
        case 0: {                                                     // 去极化
            const total = 1.0 / t1 + (t2 && t2 > 0 ? 1.0 / Math.max(t2, 1.0) : 0.0);
            return Math.min(1.0, total);
        }
        default: return DEFAULT_NOISE_PARAM;
    }
}

function isTopLevelItem(node: any): node is Item {
    if (node.type === 'VariableDeclarationList') {
        // 顶层「显式类型」多定义 → 全局变量列表；let/auto 多定义 → 脚本模式局部变量。
        return node.varType !== 'auto' && node.varType !== 'let';
    }
    return ['ModuleDecl', 'UseDecl', 'FormDecl', 'FlavorDecl', 'ImplDecl', 'TraitDecl', 'TemplateDecl', 'ImportDecl', 'RequiresDecl', 'ExternDecl', 'GlobalVarDecl'].includes(node.type);
}

interface LLVMValue {
    val: string;
    type: string;
    /** 整数符号性：false 表示无符号（uint8/16/32/64），用于选择 udiv/urem/ult/lshr 等 */
    signed?: boolean;
}

interface Scope {
    symbols: Map<string, { ptr: string, type: string, signed?: boolean }>;
    temporaries: { val: string, type: string }[];
}

interface FormField {
    name: string;
    llvmType: string;
    index: number;
    rank: string;
    /** 位域宽度（`uint8 f : 3`） */
    bitWidth?: number;
    /** 位域在存储单元内的位偏移（LSB 优先） */
    bitOffset?: number;
}

export class IRGenerator {
    private output: string[] = [];
    private globalStrings: string[] = [];
    private stringConstants: Map<string, string> = new Map();
    private stringCount: number = 1;
    private regCount: number = 1;
    private scopes: Scope[] = [{ symbols: new Map(), temporaries: [] }];
    private labelCount: number = 1;
    private loopStack: { breakLabel: string; continueLabel: string }[] = [];
    private lambdaCount: number = 1;
    private threadCount: number = 1;
    /** 已 move 消费（但符号保留）的线性资源名，emitCleanup 跳过它们 */
    private consumedSymbols: Set<string> = new Set();
    private lambdaIRs: string[] = [];
    private allocas: string[] = [];
    private isBlockTerminated: boolean = false;
    private forms: Map<string, FormDecl> = new Map();
    private traits: Map<string, TraitDecl> = new Map();
    private implMethods: Map<string, FnDecl[]> = new Map();
    private traitImpls: Map<string, Map<string, FnDecl[]>> = new Map();
    private templates: TemplateDecl[] = [];
    private flavorMembers: Map<string, number> = new Map();
    private globalVars: Map<string, GlobalVarDecl> = new Map();
    private globalVarIRs: string[] = [];
    private typeDefs: string[] = [];
    private methodIRs: string[] = [];
    private vtableConsts: string[] = [];
    private formTypeToName: Map<string, string> = new Map();
    private userFunctions: Map<string, FunctionDeclaration> = new Map();
    /** 当前函数体的噪声通道列表（@[noise]/@[coherence]），门操作后依次注入 */
    private currentNoise: { channel: number; param: number }[] | null = null;
    private importAliases: Map<string, string> = new Map();
    private importSigs: Map<string, { params: string[]; ret: string }> = new Map();
    private externSigs: Map<string, { params: string[]; ret: string; paramsQ?: string[]; retQ?: string }> = new Map();

    private nextReg(): string {
        return '%' + (this.regCount++);
    }

    private nextLabel(prefix: string): string {
        return prefix + (this.labelCount++);
    }

    private toI1(val: LLVMValue): string {
        if (val.type === 'i1') return val.val;
        const resReg = this.nextReg();
        this.emit(`${resReg} = icmp ne ${val.type} ${val.val}, 0`);
        return resReg;
    }

    private enterScope() {
        this.scopes.push({ symbols: new Map(), temporaries: [] });
    }

    private emitScopeCleanup(scope: Scope) {
        for (const [name, sym] of scope.symbols.entries()) {
            if (this.consumedSymbols.has(name)) continue; // 已 move 消费，跳过释放
            if (sym.type === '%QObject*') {
                const loadReg = this.nextReg();
                this.emit(`${loadReg} = load ${sym.type}, ${sym.type}* ${sym.ptr}`);
                this.emit(`call void @rvdl_release_object(${sym.type} ${loadReg})`);
            } else if (sym.type === '%Qubit*') {
                const loadReg = this.nextReg();
                this.emit(`${loadReg} = load ${sym.type}, ${sym.type}* ${sym.ptr}`);
                this.emit(`call void @__quantum__rt__qubit_release(${sym.type} ${loadReg})`);
            }
        }

        for (const temp of scope.temporaries) {
            if (temp.type === '%QObject*') {
                this.emit(`call void @rvdl_release_object(${temp.type} ${temp.val})`);
            } else if (temp.type === '%Qubit*') {
                this.emit(`call void @__quantum__rt__qubit_release(${temp.type} ${temp.val})`);
            }
        }
    }

    private emitCleanup() {
        for (let i = this.scopes.length - 1; i >= 0; i--) {
            this.emitScopeCleanup(this.scopes[i]);
        }
    }

    private exitScope() {
        const currentScope = this.scopes.pop();
        if (!currentScope) return;
        if (!this.isBlockTerminated) {
            this.emitScopeCleanup(currentScope);
        }
    }

    private setSymbol(name: string, data: { ptr: string, type: string, signed?: boolean }) {
        this.scopes[this.scopes.length - 1].symbols.set(name, data);
    }

    private getSymbol(name: string): { ptr: string, type: string, signed?: boolean } | undefined {
        for (let i = this.scopes.length - 1; i >= 0; i--) {
            if (this.scopes[i].symbols.has(name)) return this.scopes[i].symbols.get(name);
        }
        return undefined;
    }

    // move 消费一个线性资源（%QObject* / %Qubit*）标识符：标记为已消费，
    // emitCleanup 跳过它（避免重复释放）。符号保留，便于后续重新绑定（`x = f(x)`）。
    private consumeIfLinear(value: Expression): void {
        if (value.type === 'Identifier') {
            const sym = this.getSymbol(value.name);
            if (sym && (sym.type === '%QObject*' || sym.type === '%Qubit*')) {
                this.consumedSymbols.add(value.name);
            }
        }
    }

    // 重新绑定一个线性资源名（赋值语句 store 后调用）：清除「已消费」标记，
    // 使 emitCleanup 释放新值。
    private rebindSymbol(name: string): void {
        this.consumedSymbols.delete(name);
    }

    /** 从类型字符串判断整数是否为有符号（uint* → 无符号） */
    private isSignedType(qt: string): boolean {
        return !qt.startsWith('uint');
    }

    private trackTemporary(val: string, type: string) {
        this.scopes[this.scopes.length - 1].temporaries.push({ val, type });
    }

    private untrackTemporary(val: string) {
        const currentScope = this.scopes[this.scopes.length - 1];
        currentScope.temporaries = currentScope.temporaries.filter(t => t.val !== val);
    }

    private emit(instruction: string) {
        this.output.push(' ' + instruction);
    }

    /**
     * 浮点字面量格式化：LLVM 要求浮点常量必须带小数点。
     * JS 的 `String(1e-9)` 得到 `1e-9`（无小数点）→ LLVM 报
     * "integer/byte constant must have integer/byte type"。
     */
    private formatFloatLiteral(v: number): string {
        if (!Number.isFinite(v)) return v > 0 ? '0x7FF0000000000000' : '0xFFF0000000000000';
        let out = v.toString();
        if (out.includes('e') || out.includes('E')) {
            // 1e-9 → 1.0e-9；1e+21 → 1.0e+21（指数形式也要带小数点）
            if (!out.includes('.')) out = out.replace('e', '.0e').replace('E', '.0E');
        } else if (!out.includes('.')) {
            // 整数值浮点：360 → 360.0（否则 LLVM 判为整型常量）
            out += '.0';
        }
        if (out.startsWith('.')) out = '0' + out;
        if (out.startsWith('-.')) out = '-0' + out.slice(1);
        return out;
    }

    private addStringLiteral(str: string): string {
        return this.getOrCreateStringConstant(str);
    }

    private getOrCreateStringConstant(value: string): string {

        if (this.stringConstants.has(value)) {
            return this.stringConstants.get(value)!;
        }

        let llvmStr = "";
        // 按 UTF-8 字节处理，避免中文字符长度（字符数 ≠ 字节数）导致 IR 数组长度不匹配
        const utf8 = Buffer.from(value, 'utf8');
        for (let i = 0; i < utf8.length; i++) {
            const b = utf8[i];
            if (b === 34) llvmStr += "\\22";
            else if (b === 92) llvmStr += "\\5C";
            else if (b === 10) llvmStr += "\\0A";
            else if (b === 13) llvmStr += "\\0D";
            else if (b < 32 || b > 126) llvmStr += "\\" + b.toString(16).padStart(2, '0');
            else llvmStr += String.fromCharCode(b);
        }
        const byteLength = utf8.length + 1; // +1 for \00

        const globalName = `@.str.${this.stringCount++}`;
        const llvmStringDef = `${globalName} = private unnamed_addr constant [${byteLength} x i8] c"${llvmStr}\\00", align 1`;
        this.stringConstants.set(value, globalName);
        this.globalStrings.push(llvmStringDef);
        return globalName;
    }

    public generate(ast: Program, importSignatures?: Map<string, Map<string, { params: string[]; ret: string }>>): string {
        this.output = [];
        this.globalStrings = [];
        this.stringConstants.clear();
        this.stringCount = 1;
        this.regCount = 1;
        this.scopes = [{ symbols: new Map(), temporaries: [] }];
        this.allocas = [];
        this.isBlockTerminated = false;
        this.resetDecls();
        this.importSigs.clear();
        if (importSignatures) {
            for (const [alias, funcs] of importSignatures) {
                for (const [funcName, sig] of funcs) {
                    this.importSigs.set(alias + '::' + funcName, sig);
                }
            }
        }

        this.collectDeclarations(ast.body, '');
        this.emitGlobals();
        this.preInstantiateTemplates(ast);
        this.generateFormTypes();
        this.generateImplMethods();
        this.generateVtables();

        const header = [
            `; ModuleID = 'rvdl_module'`,
            `source_filename = "rvdl_script.rvdl"`,
            ``
        ];

        const types = [
            `; --- QIR Standard Library ---`,
            `%Qubit = type opaque`,
            `%QObject = type opaque`,
            `%QModel = type opaque`,
            `%QReservoir = type opaque`,
            `; --- 晶格数组类型（lattice<T, B>，吸收 Futhark/Remora/Rust）---`,
            `%Lattice = type opaque`,
            ``
        ];

        const declarations = [
            `declare %QObject* @rvdl_create_DiracState(i32)`,
            `declare %QObject* @rvdl_create_BellState()`,
            `declare %QObject* @rvdl_create_QuantumRegister(i32)`,
            `declare %QObject* @rvdl_create_basis_state(double, double, i32)`,
            `declare %Qubit* @__quantum__rt__qubit_allocate()`,
            `declare void @__quantum__rt__qubit_release(%Qubit*)`,
            `declare i32 @__quantum__qis__measure_int(%Qubit*)`,
            ``,
            `; --- 扩展量子门（论文3 Hadamard/QFT/MUB + 论文5 Yang-Baxter braid）---`,
            `declare void @__quantum__qis__h(%Qubit*)`,
            `declare void @__quantum__qis__x(%Qubit*)`,
            `declare void @__quantum__qis__y(%Qubit*)`,
            `declare void @__quantum__qis__z(%Qubit*)`,
            `declare void @__quantum__qis__s(%Qubit*)`,
            `declare void @__quantum__qis__t(%Qubit*)`,
            `declare void @__quantum__qis__rz(double, %Qubit*)`,
            `declare void @__quantum__qis__rx(double, %Qubit*)`,
            `declare void @__quantum__qis__ry(double, %Qubit*)`,
            `declare void @__quantum__qis__cnot(%Qubit*, %Qubit*)`,
            `declare void @__quantum__qis__toffoli(%Qubit*, %Qubit*, %Qubit*)`,
            `declare void @__quantum__qis__swap(%Qubit*, %Qubit*)`,
            `declare void @__quantum__qis__qft(i32)`,
            `declare void @__quantum__qis__iqft(i32)`,
            `declare void @__quantum__qis__cqft(%Qubit*, i32)`,
            `declare void @__quantum__qis__braid(%Qubit*, %Qubit*)`,
            `declare void @__quantum__qis__cbraid(%Qubit*, %Qubit*, %Qubit*)`,
            `; --- 受控门（可逆编织 @[steer]：cx/ch/crz/cswap）---`,
            `declare void @__quantum__qis__cx(%Qubit*, %Qubit*)`,
            `declare void @__quantum__qis__ch(%Qubit*, %Qubit*)`,
            `declare void @__quantum__qis__crz(%Qubit*, %Qubit*, double)`,
            `declare void @__quantum__qis__cswap(%Qubit*, %Qubit*, %Qubit*)`,
            `declare void @__quantum__qis__c_toffoli(%Qubit*, %Qubit*, %Qubit*, %Qubit*)`,
            `declare void @__quantum__qis__apply_noise(%Qubit*, i32, double)`,
            `declare i32 @__quantum__qis__measure_basis(%Qubit*, i8)`,
            `declare i32 @rvdl_measure_object(%QObject*)`,
            `declare %QObject* @rvdl_extract_qubit(%QObject*, i32)`,
            `declare void @rvdl_release_object(%QObject*)`,
            ``,
            `; --- QML&QKM Native Trampolines ---`,
            `declare %QObject* @rvdl_encode_text(i8*)`,
            `declare %QObject* @rvdl_encode_amplitudes(i8*, i32)`,
            `declare %QObject* @rvdl_encode_image(i8*, i32)`,
            `declare %QObject* @rvdl_encode_adaptive(i8*)`,
            `declare %QModel* @rvdl_qlm_invoke(%QObject*, i32, double)`,
            `declare void @rvdl_qkm_export(%QModel*, i8*)`,
            ``,
            `; --- Ravendrel AI Standard Library ABI ---`,
            `declare %QModel* @rvdl_qlm_load(i8*)`,
            `declare void @rvdl_qlm_forward(%QModel*, %QObject*)`,
            `declare %QObject* @rvdl_encode_string(i8*)`,
            `declare i8* @rvdl_decode_string(%QObject*)`,
            ``,
            `; --- QBNS Mind-Controlled Programming Trampolines ---`,
            `declare %QObject* @rvdl_mind_read(i8*)`,
            `declare void @rvdl_mind_train(%QObject*, i32, double)`,
            `declare void @rvdl_mind_feedback(%QObject*)`,
            ``,
            `; --- VedaROS QLM Trampolines ---`,
            `declare void @rvdl_veda_qlm_train(%QObject*, i32, double)`,
            ``,
            `; --- QRC Quantum Reservoir ABI ---`,
            `declare %QReservoir* @rvdl_qrc_new(i32, i32)`,
            `declare %QReservoir* @rvdl_qrc_new_ex(i32, i32, i32)`,
            `declare void @rvdl_qrc_train(%QReservoir*, i32, double)`,
            `declare void @rvdl_qrc_train_ex(%QReservoir*, i32, double, double*, double*, i32, i32, i32)`,
            `declare %QObject* @rvdl_qrc_probe(%QReservoir*, %QObject*)`,
            `declare %QObject* @rvdl_qrc_predict(%QReservoir*, %QObject*)`,
            `declare void @rvdl_qrc_release(%QReservoir*)`,
            ``,
            `; --- TQNF Topological Quantum Neural Field ABI ---`,
            `declare i32 @rvdl_dla_dim(i8*, i32)`,
            `declare double @rvdl_qstate_entropy(%QObject*)`,
            `declare double @rvdl_qstate_fidelity(%QObject*, %QObject*)`,
            `declare double @rvdl_qstate_purity(%QObject*)`,
            `declare double @rvdl_qstate_linear_entropy(%QObject*)`,
            `declare double @rvdl_qstate_renyi_entropy(%QObject*, double)`,
            `declare double @rvdl_math_dot(double*, double*, i32)`,
            `declare double @rvdl_math_norm(double*, i32)`,
            `declare double @rvdl_qattention(%QObject*, %QObject*)`,
            `declare double @rvdl_shannon4(i32, i32, i32, i32)`,
            `declare double @rvdl_shannon8(i32, i32, i32, i32, i32, i32, i32, i32)`,
            `declare void @rvdl_qgate_h(%QObject*, i32)`,
            `declare void @rvdl_qgate_x(%QObject*, i32)`,
            `declare void @rvdl_qgate_rz(%QObject*, i32, double)`,
            `declare void @rvdl_qgate_cnot(%QObject*, i32, i32)`,
            `declare double @rvdl_qexpect_z(%QObject*, i32)`,
            `declare i32 @rvdl_qmeasure(%QObject*, i32)`,
            `declare i32 @rvdl_qobj_num_qubits(%QObject*)`,
            `declare void @rvdl_qgate_cnot_pair(%QObject*, i32, %QObject*, i32)`,
            ``,
            `; --- Retrocausal Capacity (quantum channel back-in-time ability) ---`,
            `declare double @rvdl_retrocausal_imax(i32, double)`,
            `declare double @rvdl_retrocausal_idoe(i32, double)`,
            `declare double @rvdl_retrocausal_q_capacity(i32, double)`,
            `declare double @rvdl_retrocausal_c_capacity(i32, double)`,
            `declare double @rvdl_retrocausal_q_one_shot(i32, double, double)`,
            `declare double @rvdl_retrocausal_gain(i32, double)`,
            `declare double @rvdl_retrocausal_deformed(i32, double, double)`,
            ``,
            `; --- Retrocausal CTC (tachyon KK doubled-dimension closed timelike curve) ---`,
            `declare double @rvdl_retrocausal_ctc_q_capacity(i32, double, double, double)`,
            `declare double @rvdl_retrocausal_ctc_c_capacity(i32, double, double, double)`,
            `declare double @rvdl_retrocausal_ctc_gain(i32, double, double, double)`,
            `declare double @rvdl_retrocausal_ctc_dephasing(double)`,
            ``,
            `; --- Non-Euclidean hypersurface geodesic primitives ---`,
            `declare double @rvdl_geodesic_distance(%QObject*, %QObject*)`,
            `declare double @rvdl_inversion(double)`,
            `declare double @rvdl_hyperbolic_metric(double)`,
            `declare double @rvdl_hyperbolic_distance(double, double)`,
            ``,
            `; --- Other ---`,
            `declare double @rvdl_surrogate(double, double, double)`,
            `declare double @rvdl_tanh_quantize(double, double, i32)`,
            `declare double @rvdl_lif_step(double, double, double, double)`,
            `declare double @rvdl_mellowmax2(double, double, double)`,
            `declare double @rvdl_logsumexp2(double, double, double)`,
            `declare double @rvdl_boltzmann2(double, double, double)`,
            `declare double @rvdl_tnorm_luk(double, double)`,
            `declare double @rvdl_tnorm_prod(double, double)`,
            `declare double @rvdl_tnorm_godel(double, double)`,
            `declare double @rvdl_polymer_weight(double, double, double)`,
            `declare double @rvdl_polymer_mix_bound(double, double)`,
            `declare double @rvdl_moe_groups(double)`,
            `declare double @rvdl_moe_experts_per(double)`,
            `declare double @rvdl_moe_virtual_experts(double)`,
            `declare double @rvdl_moe_savings(double)`,
            `declare double @rvdl_curvature_gate(double)`,
            `declare double @rvdl_dual_fuse(double, double, double, double)`,
            `declare double @rvdl_tau_alpha(double)`,
            `declare double @rvdl_tau_generational(double, double)`,
            `declare double @rvdl_tau_fold_depth(double, double)`,
            `declare double @rvdl_tau_gear_ratio(double, double)`,
            `declare double @rvdl_tau_route_cost(double, double, double, double)`,
            `declare double @rvdl_tau_route_price(double, double, double)`,
            ``,
            `; --- 晶格数组 ABI（lattice<T, B>，首版 int32 元素 / 1D-2D）---`,
            `declare %Lattice* @rvdl_lattice_new(i32, i32, i32, i32)`,
            `declare void @rvdl_lattice_free(%Lattice*)`,
            `declare i32 @rvdl_lattice_ref(%Lattice*, i32, i32)`,
            `declare void @rvdl_lattice_set(%Lattice*, i32, i32, i32)`,
            `declare i32 @rvdl_lattice_rank(%Lattice*)`,
            `declare i32 @rvdl_lattice_size(%Lattice*, i32)`,
            `declare i32 @rvdl_lattice_boundary(%Lattice*)`,
            ``,
            `; --- 经典 GUI / 图形引擎 ABI（cgui_* / cgfx_*）---`,
            `declare i32 @rvdl_cgui_init(i32, i32, i8*)`,
            `declare i32 @rvdl_cgui_should_close()`,
            `declare void @rvdl_cgui_begin_frame()`,
            `declare void @rvdl_cgui_end_frame()`,
            `declare i32 @rvdl_cgui_button(i8*)`,
            `declare void @rvdl_cgui_text(i8*)`,
            `declare void @rvdl_cgui_text_int(i32)`,
            `declare void @rvdl_cgui_beep(i32, i32)`,
            `declare i32 @rvdl_cgui_width()`,
            `declare i32 @rvdl_cgui_height()`,
            `declare void @rvdl_cgui_panel(i32, i32, i32, i32, i8*)`,
            `declare void @rvdl_cgui_panel_end()`,
            `declare void @rvdl_cgui_row(i32, i32)`,
            `declare i32 @rvdl_cgui_mouse_x()`,
            `declare i32 @rvdl_cgui_mouse_y()`,
            `declare i32 @rvdl_cgui_mouse_left_clicked()`,
            `declare void @rvdl_cgfx_rect(i32, i32, i32, i32, i32)`,
            `declare void @rvdl_cgfx_line(i32, i32, i32, i32, i32, i32)`,
            `declare void @rvdl_cgfx_polyline(i32*, i32, i32, i32)`,
            `declare void @rvdl_cgfx_ellipse(i32, i32, i32, i32, i32)`,
            `declare void @rvdl_cgfx_triangle(i32, i32, i32, i32, i32, i32, i32)`,
            `declare void @rvdl_cgfx_rect_a(i32, i32, i32, i32, i32, i32)`,
            `declare void @rvdl_cgfx_line_a(i32, i32, i32, i32, i32, i32, i32)`,
            `declare void @rvdl_cgfx_polyline_a(i32*, i32, i32, i32, i32)`,
            `declare void @rvdl_cgfx_ellipse_a(i32, i32, i32, i32, i32, i32)`,
            `declare void @rvdl_cgfx_triangle_a(i32, i32, i32, i32, i32, i32, i32, i32)`,
            ``,
            `; --- QCOS Syscall ABI + Heap ---`,
            `declare i32 @rvdl_sys_call(i32, i32, i32, i32)`,
            `declare double @rvdl_sys_calld(i32, double, double)`,
            `declare void @rvdl_sys_log(i32, i8*)`,
            `declare i32 @rvdl_sys_logi(i32, i32)`,
            `declare i8* @rvdl_gc_alloc(i64)`,
            `declare i8* @rvdl_sys_callp(i32, i64, i64, i64)`,
            `declare void @rvdl_gc_free(i8*)`,
            `; --- 字符串遍历 / libc 子集 ---`,
            `declare i32 @strlen(i8*)`,
            `declare i8* @malloc(i64)`,
            `declare void @free(i8*)`,
            `declare i8* @memset(i8*, i32, i64)`,
            `declare i8* @memcpy(i8*, i8*, i64)`,
            `declare i32 @kglobals_addr()`,
            ``,
            `; --- QMS 数值内核 ---`,
            `declare double @rvdl_qms_gap(i32, double, double)`,
            `declare double @rvdl_mix_bound(double, double, double)`,
            `declare double @rvdl_qms_conc(double, double)`,
            ``,
            `; --- QChain 量子区块链 ABI ---`,
            `declare i8* @rvdl_qchain_wallet()`,
            `declare i64 @rvdl_qchain_balance(i8*)`,
            `declare void @rvdl_qchain_mint(i8*, i64)`,
            `declare i32 @rvdl_qchain_transfer(i8*, i8*, i64)`,
            `declare i32 @rvdl_qchain_mine()`,
            `declare i32 @rvdl_qchain_height()`,
            `declare i32 @rvdl_qchain_verify()`,
            `declare i8* @rvdl_qchain_qkd(i32)`,
            `declare i32 @rvdl_qchain_qdba(i32)`,
            `declare %QObject* @rvdl_qchain_coin_mint(i32)`,
            `declare i32 @rvdl_qchain_coin_verify(%QObject*)`,
            ``,
            `; --- QChain 密码原语 / 抗超时空 / 时空加密 ABI ---`,
            `declare i8* @rvdl_qchain_sha3(i8*)`,
            `declare i8* @rvdl_qchain_hmac(i8*, i8*)`,
            `declare i8* @rvdl_qchain_hash_unicode(i8*)`,
            `declare i8* @rvdl_qchain_sign(i8*)`,
            `declare i32 @rvdl_qchain_sign_verify(i8*, i8*)`,
            `declare i8* @rvdl_qchain_sign_pubkey()`,
            `declare i8* @rvdl_qchain_mlkem_encaps(i8*)`,
            `declare i8* @rvdl_qchain_mlkem_decaps(i8*, i8*)`,
            `declare i32 @rvdl_qchain_causal_verify()`,
            `declare i8* @rvdl_qchain_cipher_encrypt(i64, i8*)`,
            `declare i8* @rvdl_qchain_cipher_decrypt(i64, i8*)`,
            ``,
            `; --- 多维标签函数执行拓扑（@layer）调度 ABI ---`,
            `declare i32 @rvdl_runtime_run_topology(i8*)`,
            `; --- 并发线程（spawn）ABI ---`,
            `declare void @rvdl_spawn(i8*, i8*)`,
            ``
        ];

        const hasExplicitFunctions = ast.body.some(node => node.type === 'FunctionDeclaration');

        if (!hasExplicitFunctions) {
            this.output.push(`define i32 @rvdl_main() {`);
            this.output.push(`entry:`);
            const entryIndex = this.output.length;

            for (const node of ast.body) {
                if (isTopLevelItem(node)) continue;
                this.visitStatement(node as Statement);
            }

            this.exitScope();

            if (!this.isBlockTerminated) {
                this.emit(`ret i32 0`);
                this.isBlockTerminated = true;
            }

            this.output.push(`}`);
            this.output.splice(entryIndex, 0, ...this.allocas);
        } else {
            for (const node of ast.body) {
                if (node.type === 'FunctionDeclaration') {
                    this.visitFunctionDeclaration(node as FunctionDeclaration);
                } else if (isTopLevelItem(node)) {
                    continue;
                } else {
                    throw new Error(`IR Error: Top-level statements are not allowed when explicit functions are defined.`);
                }
            }
        }

        const topology = buildTopology(ast).topology;
        const topologyIR = this.emitTopologyIR(topology);

        const result = [
            ...header,
            ...types,
            ...this.typeDefs,
            ...this.globalStrings,
            ...(this.globalStrings.length > 0 ? [``] : []),
            ...this.globalVarIRs,
            ...(this.globalVarIRs.length > 0 ? [``] : []),
            ...declarations,
            ...this.buildImportDecls(),
            ...this.buildExternDecls(),
            ...this.vtableConsts,
            ...this.methodIRs,
            ...this.lambdaIRs,
            ...this.output,
            ...topologyIR
        ].join('\n');

        // 自校验：捕获 SSA 违反 / 基本块漏终结符 / 寄存器未定义等生成期错误
        const diagnostics = verifyIR(result);
        if (diagnostics.length > 0) {
            throw new Error('IR Verification failed:\n' +
                diagnostics.map(d => `  line ${d.line}: ${d.message}`).join('\n'));
        }

        return result;
    }

    // ---- 多维标签函数：调度表 + 拓扑入口 --------------------------------
    /** 收集所有携带 @layer 标签的函数（拓扑块） */
    private collectLayerFunctions(ast: Program): FunctionDeclaration[] {
        return ast.body.filter(
            n => n.type === 'FunctionDeclaration' && (n as FunctionDeclaration).layer
        ) as FunctionDeclaration[];
    }

    /** 把拓扑块聚合为调度表 JSON（shape + blocks + callGraph + shuttleLinks + trackingScopes） */
    private buildTopologyJson(topology: Topology): string {
        // 复用 shape（已由 TopologyBuilder 聚合）
        const shape = topology.shape;
        // blocks：仅入口块（无参数的 @layer 函数）
        const blocks: object[] = [];
        for (const b of topology.blocks) {
            const fn = topology.blocks.find(x => x.name === b.name);
            // 我们只能访问 LayerTag；寻找对应的 FunctionDeclaration 以检查 params。
            // 在此处用 name 索引回 AST，为简化直接取 fns；也可改签名传 fns 进来。
            // 这里按调用约定：buildTopologyJson 接收完整 topology，形状已聚合完毕。
            // 我们仅序列化 topology.blocks（已是精简过的）。
            blocks.push({
                name: b.name,
                time: b.layer.time ?? 0,
                thread: b.layer.thread,
                coord: b.layer.coord,
                cost: b.layer.cost ?? 1,
                deadline: b.layer.deadline ?? null,
            });
        }
        const cg = topology.callGraph.map(e => ({ caller: e.caller, callee: e.callee, startAt: e.startAt, deltaT: e.deltaT }));
        // shuttleLinks：按块名排序以保证确定性
        const sl = topology.shuttleLinks.map(l => ({
            from: l.from,
            targets: l.peers.map(p => ({
                name: p.name,
                coord: p.coord,
                logWeight: p.logWeight,
                weight: p.weight,
                distance: p.distance,
                via: p.via,
            })),
            lambda: l.lambda,
            phase: l.phase,
            rounds: l.rounds,
            strategy: l.strategy,
            slotsPerPhase: l.slotsPerPhase,
        }));
        // trackingScopes
        const ts = topology.trackingScopes.map(s => ({
            block: s.block,
            observables: s.observables,
            mode: s.mode,
            slots: s.slots,
            level: s.level,
            window: s.window,
        }));
        return JSON.stringify({ shape, blocks, callGraph: cg, shuttleLinks: sl, trackingScopes: ts });
    }

    /** 生成调度表常量 + 拓扑入口函数（有 @layer 块时才生成） */
    private emitTopologyIR(topology: Topology): string[] {
        if (topology.blocks.length === 0) return [];

        const json = this.buildTopologyJson(topology);
        const bytes = Buffer.from(json, 'utf8');
        const n = bytes.length + 1; // +1 for \00

        let llvmStr = '';
        for (let i = 0; i < bytes.length; i++) {
            const b = bytes[i];
            if (b === 34) llvmStr += '\\22';
            else if (b === 92) llvmStr += '\\5C';
            else if (b === 10) llvmStr += '\\0A';
            else if (b === 13) llvmStr += '\\0D';
            else if (b < 32 || b > 126) llvmStr += '\\' + b.toString(16).padStart(2, '0');
            else llvmStr += String.fromCharCode(b);
        }

        return [
            `; --- 多维标签函数执行拓扑（@layer）---`,
            `@rvdl_topology_json = private unnamed_addr constant [${n} x i8] c"${llvmStr}\\00", align 1`,
            ``,
            `define i32 @rvdl_topology_entry() {`,
            `entry:`,
            `  %0 = call i32 @rvdl_runtime_run_topology(i8* getelementptr inbounds ([${n} x i8], [${n} x i8]* @rvdl_topology_json, i64 0, i64 0))`,
            `  ret i32 %0`,
            `}`,
        ];
    }

    private buildImportDecls(): string[] {
        const decls: string[] = [];
        const formTypes = new Set<string>();
        for (const [alias, mmiPath] of this.importAliases) {
            for (const [key, sig] of this.importSigs) {
                if (key.startsWith(alias + '::')) {
                    const funcName = key.slice(alias.length + 2);
                    const symbol = alias + '_' + funcName;
                    decls.push(`declare ${sig.ret} @${symbol}(${sig.params.join(', ')})`);
                    // 收集 form 类型（%form.X*），生成 opaque 定义供 declare 使用
                    for (const t of [sig.ret, ...sig.params]) {
                        const m = t.match(/^%form\.([A-Za-z0-9_]+)\*$/);
                        if (m) formTypes.add(m[1]);
                    }
                }
            }
        }
        const defs: string[] = [];
        for (const ft of formTypes) {
            defs.push(`%form.${ft} = type opaque`);
        }
        return [...defs, ...decls];
    }

    // 外部 C 符号（extern 声明）的 declare
    private buildExternDecls(): string[] {
        const decls: string[] = [];
        for (const [name, sig] of this.externSigs) {
            decls.push(`declare ${sig.ret} @${name}(${sig.params.join(', ')})`);
        }
        return decls;
    }

    private resetDecls() {
        this.forms.clear();
        this.traits.clear();
        this.implMethods.clear();
        this.traitImpls.clear();
        this.templates = [];
        this.typeDefs = [];
        this.methodIRs = [];
        this.vtableConsts = [];
        this.lambdaIRs = [];
        this.userFunctions.clear();
        this.importAliases.clear();
        this.externSigs.clear();
    }

    private collectDeclarations(items: (Statement | Item)[], prefix: string) {
        for (const node of items) {
            if (node.type === 'FormDecl') {
                const fullName = prefix ? prefix + '::' + node.name : node.name;
                this.forms.set(fullName, node);
            } else if (node.type === 'FlavorDecl') {
                let next = 0;
                for (const m of node.members) {
                    const value = m.value ?? next;
                    this.flavorMembers.set(m.name, value);
                    next = value + 1;
                }
            } else if (node.type === 'GlobalVarDecl') {
                const fullName = prefix ? prefix + '::' + node.name : node.name;
                this.globalVars.set(fullName, node);
            } else if (node.type === 'VariableDeclarationList' && node.varType !== 'auto' && node.varType !== 'let') {
                // 一行多定义（顶层全局变量列表）：逐个降为 GlobalVarDecl 注册
                for (const d of node.declarations) {
                    const fullName = prefix ? prefix + '::' + d.identifier : d.identifier;
                    const g: GlobalVarDecl = {
                        type: 'GlobalVarDecl',
                        name: d.identifier,
                        varType: d.varType,
                        isFixed: d.isFixed ?? false,
                        value: d.value,
                        isPub: false,
                        line: d.line,
                        column: d.column,
                        length: d.length
                    };
                    this.globalVars.set(fullName, g);
                }
            } else if (node.type === 'TraitDecl') {
                const fullName = prefix ? prefix + '::' + node.name : node.name;
                this.traits.set(fullName, node);
            } else if (node.type === 'ImplDecl') {
                if (node.traitName) {
                    if (!this.traitImpls.has(node.traitName)) this.traitImpls.set(node.traitName, new Map());
                    this.traitImpls.get(node.traitName)!.set(node.target, node.methods);
                } else {
                    if (!this.implMethods.has(node.target)) this.implMethods.set(node.target, []);
                    this.implMethods.get(node.target)!.push(...node.methods);
                }
            } else if (node.type === 'TemplateDecl') {
                this.templates.push(node);
            } else if (node.type === 'FunctionDeclaration') {
                this.userFunctions.set(node.name, node);
            } else if (node.type === 'ImportDecl') {
                this.importAliases.set(node.alias, node.path);
            } else if (node.type === 'ExternDecl') {
                this.externSigs.set(node.name, {
                    params: node.params.map(p => this.getLLVMType(p.type)),
                    ret: this.getLLVMType(node.returnType),
                    // 同时保存 rvdl 侧的原始类型：uint32 与 int32 在 LLVM 里
                    // 都是 i32，只有这里能恢复"无符号"语义（urem/lshr/ult）
                    paramsQ: node.params.map(p => p.type),
                    retQ: node.returnType
                });
            } else if (node.type === 'ModuleDecl') {
                const fullName = prefix ? prefix + '::' + node.name : node.name;
                this.collectDeclarations(node.body, fullName);
            }
        }
    }

    /** 根据 LLVM 类型返回正确的全局变量对齐（字节）。 */
    private typeAlign(llvmType: string): number {
        if (llvmType === 'double' || llvmType === 'i64' || llvmType === 'uint64') return 8;
        if (llvmType.endsWith('*')) return 8;
        if (llvmType === 'float' || llvmType === 'i32' || llvmType === 'uint32') return 4;
        if (llvmType === 'i16' || llvmType === 'uint16') return 2;
        return 1;
    }

    /** 生成全局变量 / 顶层 fixed 常量的 LLVM global 定义。 */
    private emitGlobals(): void {
        this.globalVarIRs = [];
        for (const [name, decl] of this.globalVars) {
            const llvmType = this.getLLVMType(decl.varType);
            const init = this.constInitValue(decl.value, llvmType);
            const linkage = decl.isFixed ? 'constant' : 'global';
            this.globalVarIRs.push(`@${name} = ${linkage} ${llvmType} ${init}, align ${this.typeAlign(llvmType)}`);
        }
    }

    /** 全局变量初始值的编译期常量求值（仅支持字面量；复杂表达式降级为 0）。 */
    /** 把常量表达式 AST 折成数值；无法折叠时返回 null。 */
    private foldConst(node: any): number | null {
        if (!node) return null;
        if (node.type === 'NumberLiteral') return node.value;
        if (node.type === 'BoolLiteral') return node.value ? 1 : 0;
        if (node.type === 'CharLiteral') return node.value.charCodeAt(0);
        if (node.type === 'Identifier') {
            const g = this.globalVars.get(node.name);
            if (g && g.isFixed && g.value) return this.foldConst(g.value);
            return null;
        }
        if (node.type === 'UnaryExpression') {
            const v = this.foldConst(node.operand);
            if (v === null) return null;
            switch (node.operator) {
                case '-': return -v;
                case '+': return v;
                case '~': return ~v;
                default: return null;
            }
        }
        if (node.type === 'BinaryExpression') {
            const l = this.foldConst(node.left);
            const r = this.foldConst(node.right);
            if (l === null || r === null) return null;
            switch (node.operator) {
                case '+': return l + r;
                case '-': return l - r;
                case '*': return l * r;
                case '/': return r === 0 ? null : Math.trunc(l / r);
                case '%': return r === 0 ? null : l % r;
                case '&': return l & r;
                case '|': return l | r;
                case '^': return l ^ r;
                case '<<': return l << r;
                case '>>': return l >> r;
                default: return null;
            }
        }
        return null;
    }

    /** 按目标 LLVM 类型把数值格式化成常量初始化字符串。 */
    private literalFromNumber(v: number, llvmType: string): string {
        if (llvmType === 'double') {
            const s = this.formatFloatLiteral(v);
            return s.endsWith('.0') ? s : s;
        }
        if (llvmType === 'float') {
            return this.formatFloatLiteral(v);
        }
        return String(v | 0);
    }

    private constInitValue(value: any, llvmType: string): string {
        if (value.type === 'NumberLiteral') {
            if (value.isFloat) {
                return this.formatFloatLiteral(value.value);
            }
            // 指针类型（cap<T>）的 0 初始值 → null（空指针），而非整型 0
            if (llvmType.endsWith('*') && value.value === 0) return 'null';
            return value.value.toString();
        }
        if (value.type === 'BoolLiteral') {
            return value.value ? 'true' : 'false';
        }
        if (value.type === 'CharLiteral') {
            return String(value.value.charCodeAt(0));
        }
        if (value.type === 'ArrayLiteral') {
            // 数组字面量常量：[e0, e1, ...] → LLVM 数组常量 [T v0, T v1, ...]
            const m = llvmType.match(/^\[(\d+) x (.+)\]$/);
            const elemType = m ? m[2] : 'i32';
            const elems = value.elements.map((e: any) => `${elemType} ${this.constInitValue(e, elemType)}`).join(', ');
            return `[${elems}]`;
        }
        // 引用另一个常量/全局（`fixed int32 A = B;`）—— 必须**转发**它的值，
        // 不能降级为 0。
        //
        // 缺陷背景（实测）：qcap.rvdl 里写 `fixed int32 QCAP_MAX_PROC = QMEM_QCAP_MAX;`
        // 是很自然的写法（把布局表当唯一分配权威），但此前会生成
        //   @QCAP_MAX_PROC = constant i32 0
        // 于是所有边界判断（`pid >= QCAP_MAX_PROC`）恒真 → 能力模型整层失效，
        // 而且**不报任何错**。这是最危险的一类缺陷：静默改变语义。
        if (value.type === 'Identifier') {
            const src = this.globalVars.get(value.name);
            if (src && src.value) {
                return this.constInitValue(src.value, llvmType);
            }
            return llvmType === 'double' ? '0.0' : '0';
        }
        // 常量表达式（UnaryExpression / BinaryExpression）：折叠成字面量。
        //
        // 缺陷背景（实测）：`fixed int32 QDEV_ERR_NODEV = 0 - 1;` 这类写法
        // （rvdl 没有负数字面量，习惯写成 0 - 1）此前会静默变成
        //   @QDEV_ERR_NODEV = constant i32 0
        // 于是所有错误码都是 0，错误分类与重试策略全部失效且无任何报错。
        if (value.type === 'UnaryExpression') {
            const operand = this.foldConst(value.operand);
            if (operand !== null) {
                switch (value.operator) {
                    case '-': return this.literalFromNumber(-operand, llvmType);
                    case '+': return this.literalFromNumber(operand, llvmType);
                    case '!': return operand === 0 ? 'true' : 'false';
                    case '~': return this.literalFromNumber(~operand, llvmType);
                }
            }
        }
        if (value.type === 'BinaryExpression') {
            const l = this.foldConst(value.left);
            const r = this.foldConst(value.right);
            if (l !== null && r !== null) {
                let v: number;
                switch (value.operator) {
                    case '+': v = l + r; break;
                    case '-': v = l - r; break;
                    case '*': v = l * r; break;
                    case '/': v = r === 0 ? 0 : Math.trunc(l / r); break;
                    case '%': v = r === 0 ? 0 : l % r; break;
                    case '&': v = l & r; break;
                    case '|': v = l | r; break;
                    case '^': v = l ^ r; break;
                    case '<<': v = l << r; break;
                    case '>>': v = l >> r; break;
                    default: v = null as any;
                }
                if (v !== null && v !== undefined && !isNaN(v)) {
                    return this.literalFromNumber(v, llvmType);
                }
            }
        }
        // 其他（字符串/表达式）：降级为 0 / null（指针类型用 null 明确空指针语义）
        if (llvmType.endsWith('*')) return 'null';
        return llvmType === 'double' ? '0.0' : '0';
    }

    // 函数默认返回值：指针 → null，double/float → 0.0，其他 → 0。
    private defaultRetValue(llvmType: string): string {
        if (llvmType.endsWith('*')) return 'null';
        if (llvmType === 'double' || llvmType === 'float') return '0.0';
        return '0';
    }

    private mangleForm(name: string): string {
        return 'form.' + name.replace(/::/g, '__');
    }

    private mangleMethod(formName: string, methodName: string): string {
        return formName.replace(/::/g, '__') + '_' + methodName;
    }

    private getFormFields(formName: string): FormField[] {
        const form = this.forms.get(formName);
        if (!form) return [];

        const packed = form.packed === true;
        const fields: FormField[] = [];
        if (form.inherits) {
            const parentFields = this.getFormFields(form.inherits.base);
            const filter = form.inherits.ranks;
            for (const pf of parentFields) {
                if (filter.length === 0 || filter.includes(pf.rank)) {
                    fields.push({ ...pf });
                }
            }
        }

        // packed form：无 vtable，字段从 0 起；普通 form：field 0 是 vtable 指针。
        let storageIndex = packed ? fields.length : fields.length + 1;
        // 位域打包：连续同类型位域合并进同一存储单元（LSB 优先，超宽自动开新单元）
        let storage: { llvmType: string; usedBits: number } | null = null;
        for (const rank of form.ranks) {
            for (const f of rank.fields) {
                const llvmType = this.getLLVMFieldType(f.type);
                const bw = f.bitWidth;
                if (bw !== undefined && bw > 0) {
                    const typeBits = this.typeBitWidth(llvmType);
                    if (!storage || storage.llvmType !== llvmType || storage.usedBits + bw > typeBits) {
                        storage = { llvmType, usedBits: 0 };
                        fields.push({ name: f.name, llvmType, index: storageIndex++, rank: rank.name, bitWidth: bw, bitOffset: 0 });
                    } else {
                        fields.push({ name: f.name, llvmType, index: storageIndex - 1, rank: rank.name, bitWidth: bw, bitOffset: storage.usedBits });
                    }
                    storage.usedBits += bw;
                } else {
                    storage = null;
                    fields.push({ name: f.name, llvmType, index: storageIndex++, rank: rank.name });
                }
            }
        }
        return fields;
    }

    /** 整数 LLVM 类型的位宽（位域打包用） */
    private typeBitWidth(llvmType: string): number {
        switch (llvmType) {
            case 'i8': return 8;
            case 'i16': return 16;
            case 'i32': return 32;
            case 'i64': return 64;
            default: return 32;
        }
    }

    private instantiateTemplate(className: string): string | null {
        const match = className.match(/^([\w]+)<(.+)>$/);
        if (!match) return null;
        const templateName = match[1];
        const typeArg = match[2];

        const template = this.templates.find(t => t.inner.type === 'FormDecl' && t.inner.name === templateName);
        if (!template) return null;

        const instName = templateName + '_' + typeArg;
        if (this.forms.has(instName)) return instName;

        const formDecl = template.inner as FormDecl;
        const instForm: FormDecl = {
            ...formDecl,
            name: instName,
            ranks: formDecl.ranks.map(rank => ({
                ...rank,
                fields: rank.fields.map(f => ({
                    ...f,
                    type: f.type === template.params[0] ? typeArg : f.type
                }))
            }))
        };
        this.forms.set(instName, instForm);
        return instName;
    }

    private preInstantiateTemplates(ast: Program) {
        const visit = (node: any) => {
            if (!node || typeof node !== 'object') return;
            if (node.type === 'NewExpression' && typeof node.className === 'string' && node.className.includes('<')) {
                this.instantiateTemplate(node.className);
            }
            for (const key of Object.keys(node)) {
                if (key === 'line' || key === 'column' || key === 'length') continue;
                const child = node[key];
                if (Array.isArray(child)) child.forEach(visit);
                else if (child && typeof child === 'object') visit(child);
            }
        };
        visit(ast);
    }

    private generateFormTypes() {
        for (const [formName, form] of this.forms) {
            const fields = this.getFormFields(formName);
            // 位域组：同一 index 的多个位域共享一个存储单元，结构类型按唯一 index 去重
            const seen = new Set<number>();
            const storageFields = fields.filter(f => {
                if (seen.has(f.index)) return false;
                seen.add(f.index);
                return true;
            });
            const typeName = this.mangleForm(formName);
            if (form.packed) {
                // packed：无 vtable、无填充，packed struct 字面量 <{ ... }>
                const body = storageFields.map(f => f.llvmType);
                this.typeDefs.push(`%${typeName} = type <{ ${body.join(', ')} }>`);
            } else {
                const body = ['i8*', ...storageFields.map(f => f.llvmType)];
                this.typeDefs.push(`%${typeName} = type { ${body.join(', ')} }`);
            }
            this.formTypeToName.set(typeName, formName);
        }
        this.typeDefs.push('');
    }

    private generateVtables() {
        for (const [traitName, impls] of this.traitImpls) {
            const trait = this.traits.get(traitName);
            if (!trait) continue;
            const methodNames: string[] = [];
            for (const rank of trait.ranks) {
                for (const m of rank.methods) methodNames.push(m.name);
            }
            if (methodNames.length === 0) continue;

            const vtableType = `%${this.mangleForm(traitName)}.vtable`;
            const slots = methodNames.map(() => `i8*`);
            this.typeDefs.push(`${vtableType} = type { ${slots.join(', ')} }`);

            for (const [formName, methods] of impls) {
                const implByName = new Map(methods.map(m => [m.name, m]));
                const entries = methodNames.map(n => {
                    const m = implByName.get(n);
                    if (!m) return 'i8* null';
                    const paramTypes = m.params.map(p => this.getLLVMType(p.type));
                    const selfType = '%' + this.mangleForm(formName) + '*';
                    const fnType = `${this.getLLVMType(m.returnType)} (${[selfType, ...paramTypes].join(', ')})*`;
                    return `i8* bitcast (${fnType} @${this.mangleMethod(formName, n)} to i8*)`;
                });
                this.vtableConsts.push(`@${this.mangleForm(formName)}.vtable = constant ${vtableType} { ${entries.join(', ')} }`);
            }
        }
        if (this.vtableConsts.length > 0) this.vtableConsts.push('');
    }

    private generateImplMethods() {
        for (const [formName, methods] of this.implMethods) {
            for (const m of methods) this.generateMethodFunction(formName, m);
        }
        for (const [, impls] of this.traitImpls) {
            for (const [formName, methods] of impls) {
                for (const m of methods) this.generateMethodFunction(formName, m);
            }
        }
    }

    private generateMethodFunction(formName: string, m: FnDecl) {
        if (!m.body) return;

        const selfType = '%' + this.mangleForm(formName) + '*';
        const retType = this.getLLVMType(m.returnType);
        const paramTypes = m.params.map(p => this.getLLVMType(p.type));
        const savedOutput = this.output;
        const savedScopes = this.scopes;
        const savedAllocas = this.allocas;
        const savedReg = this.regCount;
        const savedTerminated = this.isBlockTerminated;
        const savedLabel = this.labelCount;
        this.output = [];
        this.scopes = [{ symbols: new Map(), temporaries: [] }];
        this.allocas = [];
        this.regCount = 1;
        this.isBlockTerminated = false;
        const fullTypes = [selfType, ...paramTypes];
        const paramsStr = fullTypes.map((t, i) => `${t} %arg${i}`).join(', ');
        const sig = `define ${retType} @${this.mangleMethod(formName, m.name)}(${paramsStr})`;
        this.methodIRs.push('');
        this.methodIRs.push(sig + ' {');
        this.methodIRs.push('entry:');
        const entryIndex = this.output.length;
        const selfPtr = '%self_ptr';
        this.allocas.push(` ${selfPtr} = alloca ${selfType}`);
        this.emit(`store ${selfType} %arg0, ${selfType}* ${selfPtr}`);
        this.setSymbol('self', { ptr: selfPtr, type: selfType });
        m.params.forEach((p, i) => {
            const ptr = '%' + p.name + '_ptr';
            this.allocas.push(` ${ptr} = alloca ${paramTypes[i]}`);
            this.emit(`store ${paramTypes[i]} %arg${i + 1}, ${paramTypes[i]}* ${ptr}`);
            this.setSymbol(p.name, { ptr: ptr, type: paramTypes[i], signed: this.isSignedType(p.type) });
        });

        for (const stmt of m.body) {
            this.visitStatement(stmt);
        }
        this.exitScope();
        if (!this.isBlockTerminated) {
            if (retType === 'void') this.emit('ret void');
            else this.emit(`ret ${retType} ${this.defaultRetValue(retType)}`);
            this.isBlockTerminated = true;
        }

        this.output.splice(entryIndex, 0, ...this.allocas);
        this.methodIRs.push(...this.output);
        this.methodIRs.push('}');
        this.output = savedOutput;
        this.scopes = savedScopes;
        this.allocas = savedAllocas;
        this.regCount = savedReg;
        this.isBlockTerminated = savedTerminated;
        this.labelCount = savedLabel;
    }

    private generateLambdaFunction(lambdaName: string, params: Param[], returnType: string, body: Statement[], captured: string[], capTypes: string[]): void {
        const llvmRetType = this.getLLVMType(returnType);
        const paramTypes = params.map(p => this.getLLVMType(p.type));

        const savedOutput = this.output;
        const savedScopes = this.scopes;
        const savedAllocas = this.allocas;
        const savedReg = this.regCount;
        const savedTerminated = this.isBlockTerminated;
        const savedLabel = this.labelCount;
        this.output = [];
        this.scopes = [{ symbols: new Map(), temporaries: [] }];
        this.allocas = [];
        this.regCount = 1;
        this.isBlockTerminated = false;

        // thunk 签名：ret @lambda_N(i8* %env, args...)
        const closureTypeName = 'closure.' + lambdaName;
        const paramsStr = ['i8* %env', ...paramTypes.map((t, i) => `${t} %arg${i}`)].join(', ');
        this.lambdaIRs.push('');
        this.lambdaIRs.push(`define ${llvmRetType} @${lambdaName}(${paramsStr}) {`);
        this.lambdaIRs.push('entry:');
        const entryIndex = this.output.length;

        // 从 env（闭包对象）读捕获变量
        if (captured.length > 0) {
            const envTyped = this.nextReg();
            this.emit(`${envTyped} = bitcast i8* %env to %${closureTypeName}*`);
            captured.forEach((capName, i) => {
                const fieldPtr = this.nextReg();
                this.emit(`${fieldPtr} = getelementptr %${closureTypeName}, %${closureTypeName}* ${envTyped}, i32 0, i32 ${i + 1}`);
                const capPtr = '%' + capName + '_cap_ptr';
                this.allocas.push(` ${capPtr} = alloca ${capTypes[i]}`);
                const capVal = this.nextReg();
                this.emit(`${capVal} = load ${capTypes[i]}, ${capTypes[i]}* ${fieldPtr}`);
                this.emit(`store ${capTypes[i]} ${capVal}, ${capTypes[i]}* ${capPtr}`);
                this.setSymbol(capName, { ptr: capPtr, type: capTypes[i] });
            });
        }

        params.forEach((p, i) => {
            const ptr = '%' + p.name + '_ptr';
            this.allocas.push(` ${ptr} = alloca ${paramTypes[i]}`);
            this.emit(`store ${paramTypes[i]} %arg${i}, ${paramTypes[i]}* ${ptr}`);
            this.setSymbol(p.name, { ptr: ptr, type: paramTypes[i] });
        });

        for (const stmt of body) {
            this.visitStatement(stmt);
        }
        this.exitScope();
        if (!this.isBlockTerminated) {
            if (llvmRetType === 'void') this.emit('ret void');
            else this.emit(`ret ${llvmRetType} ${this.defaultRetValue(llvmRetType)}`);
            this.isBlockTerminated = true;
        }

        this.output.splice(entryIndex, 0, ...this.allocas);
        this.lambdaIRs.push(...this.output);
        this.lambdaIRs.push('}');

        this.output = savedOutput;
        this.scopes = savedScopes;
        this.allocas = savedAllocas;
        this.regCount = savedReg;
        this.isBlockTerminated = savedTerminated;
        this.labelCount = savedLabel;
    }

    // spawn 线程函数：独立函数 @rvdl_thread_N(i8* %env)，经 rvdl_spawn 内建以 std::thread 启动。
    // 捕获变量经 env 闭包结构传递（复用 lambda 的闭包机制）。
    private generateThreadFunction(name: string, body: Statement[], captured: string[], capTypes: string[]): void {
        const savedOutput = this.output;
        const savedScopes = this.scopes;
        const savedAllocas = this.allocas;
        const savedReg = this.regCount;
        const savedTerminated = this.isBlockTerminated;
        const savedLabel = this.labelCount;
        this.output = [];
        this.scopes = [{ symbols: new Map(), temporaries: [] }];
        this.allocas = [];
        this.regCount = 1;
        this.isBlockTerminated = false;

        const envTypeName = 'env.' + name;
        this.lambdaIRs.push('');
        this.lambdaIRs.push(`define void @${name}(i8* %env) {`);
        this.lambdaIRs.push('entry:');
        const entryIndex = this.output.length;

        // 从 env 读捕获变量
        if (captured.length > 0) {
            const envTyped = this.nextReg();
            this.emit(`${envTyped} = bitcast i8* %env to %${envTypeName}*`);
            captured.forEach((capName, i) => {
                const fieldPtr = this.nextReg();
                this.emit(`${fieldPtr} = getelementptr %${envTypeName}, %${envTypeName}* ${envTyped}, i32 0, i32 ${i}`);
                const capPtr = '%' + capName + '_cap_ptr';
                this.allocas.push(` ${capPtr} = alloca ${capTypes[i]}`);
                const capVal = this.nextReg();
                this.emit(`${capVal} = load ${capTypes[i]}, ${capTypes[i]}* ${fieldPtr}`);
                this.emit(`store ${capTypes[i]} ${capVal}, ${capTypes[i]}* ${capPtr}`);
                this.setSymbol(capName, { ptr: capPtr, type: capTypes[i] });
            });
        }

        for (const s of body) {
            this.visitStatement(s);
        }
        this.exitScope();
        if (!this.isBlockTerminated) {
            this.emit('ret void');
            this.isBlockTerminated = true;
        }

        this.output.splice(entryIndex, 0, ...this.allocas);
        this.lambdaIRs.push(...this.output);
        this.lambdaIRs.push('}');

        this.output = savedOutput;
        this.scopes = savedScopes;
        this.allocas = savedAllocas;
        this.regCount = savedReg;
        this.isBlockTerminated = savedTerminated;
        this.labelCount = savedLabel;
    }

    // 收集 lambda 的自由变量（得要捕获的外部变量）
    private collectFreeVariables(body: Statement[], params: Param[]): string[] {
        const bound = new Set<string>(params.map(p => p.name));
        const free: string[] = [];
        const seen = new Set<string>();

        const markFree = (name: string) => {
            if (!bound.has(name) && !seen.has(name)) {
                seen.add(name);
                free.push(name);
            }
        };

        const visitExpr = (e: Expression) => {
            if (!e) return;
            switch (e.type) {
                case 'Identifier':
                    markFree(e.name);
                    break;
                case 'BinaryExpression':
                    visitExpr(e.left); visitExpr(e.right); break;
                case 'LogicalExpression':
                    visitExpr(e.left); visitExpr(e.right); break;
                case 'UnaryExpression':
                    visitExpr(e.argument); break;
                case 'FunctionCall':
                    e.arguments.forEach(visitExpr);
                    break;
                case 'MemberExpression':
                    visitExpr(e.object);
                    e.arguments.forEach(visitExpr);
                    break;
                case 'NewExpression':
                    e.arguments.forEach(visitExpr);
                    break;
                case 'IndexExpression':
                    visitExpr(e.object);
                    e.indices.forEach(visitExpr);
                    break;
            }
        };

        const visitStmt = (s: Statement) => {
            switch (s.type) {
                case 'VariableDeclaration':
                    visitExpr(s.value);
                    bound.add(s.identifier);
                    break;
                case 'VariableDeclarationList':
                    for (const d of s.declarations) {
                        visitExpr(d.value);
                        bound.add(d.identifier);
                    }
                    break;
                case 'AssignmentStatement':
                    markFree(s.name);
                    visitExpr(s.value);
                    break;
                case 'ExpressionStatement':
                    visitExpr(s.expression);
                    break;
                case 'ReturnStatement':
                    visitExpr(s.argument);
                    break;
                case 'WhileStatement':
                    visitExpr(s.condition);
                    s.body.forEach(visitStmt);
                    if (s.elseBody) s.elseBody.forEach(visitStmt);
                    break;
                case 'IfStatement':
                    visitExpr(s.condition);
                    s.consequent.forEach(visitStmt);
                    if (s.alternate) s.alternate.forEach(visitStmt);
                    break;
                case 'UnsafeBlock':
                    s.body.forEach(visitStmt);
                    break;
                case 'ForStatement':
                    if (s.init) visitStmt(s.init);
                    if (s.condition) visitExpr(s.condition);
                    s.body.forEach(visitStmt);
                    if (s.update) visitStmt(s.update);
                    break;
            }
        };

        body.forEach(visitStmt);
        return free;
    }

    private getLLVMFieldType(ravendrelType: string): string {
        return this.getLLVMType(ravendrelType);
    }

    private formSizeBytes(formName: string): number {
        // packed form：无 vtable，字段字节求和；普通 form：先计入 vtable 指针（8 字节）
        const packed = this.forms.get(formName)?.packed === true;
        let size = packed ? 0 : 8;
        for (const f of this.getFormFields(formName)) {
            size += this.typeSize(f.llvmType);
        }
        return size;
    }

    private typeSize(t: string): number {
        switch (t) {
            case 'i8': return 1;
            case 'i16': return 2;
            case 'i32': return 4;
            case 'i64': return 8;
            case 'float': return 4;
            case 'double': return 8;
            case 'i1': return 1;
            case 'i8*': return 8;
            default:
                if (t.startsWith('[') && t.endsWith(']')) {
                    const m = t.match(/^\[(\d+) x (.+)\]$/);
                    if (m) return parseInt(m[1], 10) * this.typeSize(m[2]);
                }
                if (t.endsWith('*')) return 8; // 指针 / 函数指针（i32 (i32)*）
                return 8;
        }
    }

    // 边界条件字符串 -> 数值编码（与 C++ 运行时约定一致）
    private boundaryCode(b: string): number {
        switch (b.trim()) {
            case 'periodic': return 1;
            case 'reflect': return 2;
            default: return 0; // open
        }
    }

    /**
     * 存储前类型对齐：把 rhs 转换为 targetType（仅处理 int<->float 与整数宽度）。
     * 缺这条路径时，`int32 x = 1.5 * 2.0` 会生成 `store i32 %doubleReg` 的非法 IR。
     */
    private coerceForStore(targetType: string, rhs: LLVMValue, targetSigned: boolean): string {
        if (targetType === rhs.type) return rhs.val;
        const isFloatT = targetType === 'double' || targetType === 'float';
        const isFloatR = rhs.type === 'double' || rhs.type === 'float';
        const intWidths: Record<string, number> = { i1: 1, i8: 8, i16: 16, i32: 32, i64: 64 };
        const tgtPtr = targetType.endsWith('*');
        const srcPtr = rhs.type.endsWith('*');
        // int（地址）→ cap<T>：整型地址构造能力
        if (tgtPtr && !srcPtr && rhs.val !== 'null') {
            const t = this.nextReg();
            this.emit(`${t} = inttoptr ${rhs.type} ${rhs.val} to ${targetType}`);
            return t;
        }
        // cap<T1> → cap<T2>：指针元素类型重解释（null 为 typeless 常量，无需转换）
        if (tgtPtr && srcPtr && rhs.val !== 'null') {
            const t = this.nextReg();
            this.emit(`${t} = bitcast ${rhs.type} ${rhs.val} to ${targetType}`);
            return t;
        }
        // float -> int：截断（rvdl 语义：隐式赋值即截断，与 C 静态转换一致）
        if (isFloatR && intWidths[targetType] !== undefined) {
            const t = this.nextReg();
            this.emit(`${t} = fptosi ${rhs.type} ${rhs.val} to ${targetType}`);
            return t;
        }
        // int -> float
        if (isFloatT && intWidths[rhs.type] !== undefined) {
            const t = this.nextReg();
            this.emit(`${t} = ${rhs.signed === false ? 'uitofp' : 'sitofp'} ${rhs.type} ${rhs.val} to ${targetType}`);
            return t;
        }
        // 整数宽度
        const pw = intWidths[targetType];
        const rw = intWidths[rhs.type];
        if (pw !== undefined && rw !== undefined) {
            if (rw > pw) {
                const t = this.nextReg();
                this.emit(`${t} = trunc ${rhs.type} ${rhs.val} to ${targetType}`);
                return t;
            }
            if (rw < pw) {
                const t = this.nextReg();
                this.emit(`${t} = ${rhs.signed === false || !targetSigned ? 'zext' : 'sext'} ${rhs.type} ${rhs.val} to ${targetType}`);
                return t;
            }
        }
        return rhs.val;
    }

    /**
     * 调用实参按被调函数的**声明形参类型**做隐式数值转换。
     *
     * 缺陷背景（实测）：rvdl 允许把 double 实参传给 int32 形参（赋值路径走
     * coerceForStore 已有处理），但调用路径直接用 `${paramType} ${argVal}`
     * 拼指令，不做转换，于是生成
     *   %1 = call i32 @f(double %0)     <- LLVM 类型不匹配，clang 报错
     * 同一缺陷也影响 extern C 调用与 import 调用。
     */
    private coerceArgs(paramTypes: string[], args: LLVMValue[]): LLVMValue[] {
        return args.map((a, i) => {
            const want = paramTypes[i];
            if (!want || want === a.type) return a;
            if (a.type === 'void') return a;
            const coerced = this.coerceForStore(want, a, true);
            return { val: coerced, type: want };
        });
    }

    private visitStatement(stmt: Statement) {
        if (this.isBlockTerminated) return;
        if (stmt.type === 'VariableDeclarationList') {
            for (const d of stmt.declarations) this.visitStatement(d);
            return;
        }
        if (stmt.type === 'VariableDeclaration') {
            // 数组字面量初始化：alloca [N x T]，逐元素 store（定长数组 / 静态查找表）
            if (stmt.value.type === 'ArrayLiteral') {
                const raw = stmt.varType.startsWith('arr<') ? stmt.varType.slice(4, -1).split(',') : null;
                const elemType = raw ? this.getLLVMType(raw[0].trim()) : 'i32';
                const len = raw ? raw[1].trim() : String(stmt.value.elements.length);
                const llvmType = `[${len} x ${elemType}]`;
                const ptrReg = '%' + stmt.identifier + '_ptr_' + (this.labelCount++);
                this.allocas.push(` ${ptrReg} = alloca ${llvmType}`);
                stmt.value.elements.forEach((e, i) => {
                    const ev = this.visitExpression(e);
                    const gep = this.nextReg();
                    this.emit(`${gep} = getelementptr ${llvmType}, ${llvmType}* ${ptrReg}, i32 0, i32 ${i}`);
                    this.emit(`store ${ev.type} ${ev.val}, ${ev.type}* ${gep}`);
                });
                this.setSymbol(stmt.identifier, { ptr: ptrReg, type: llvmType });
                return;
            }
            const rhs = this.visitExpression(stmt.value);
            this.untrackTemporary(rhs.val);
            const llvmType = stmt.varType === 'auto' ? rhs.type : this.getLLVMType(stmt.varType);
            const ptrReg = '%' + stmt.identifier + '_ptr_' + (this.labelCount++);
            this.allocas.push(` ${ptrReg} = alloca ${llvmType}`);

            // int -> cap：整型地址构造能力（inttoptr）
            let storedVal = rhs.val;
            if (llvmType !== rhs.type && llvmType.endsWith('*') && rhs.type.endsWith(')*')) {
                // 函数指针 -> 原始字节指针（cap<uint8>，供中断表 / IDT / 回调表存储）：bitcast
                const castReg = this.nextReg();
                this.emit(`${castReg} = bitcast ${rhs.type} ${rhs.val} to ${llvmType}`);
                storedVal = castReg;
            }
            else if (llvmType !== rhs.type && llvmType.endsWith('*') && rhs.type.endsWith('*') && rhs.val !== 'null') {
                // cap<T1> -> cap<T2>：指针元素类型重解释（如 cap<int32> -> cap<int8>，
                // rvdl_gc_alloc 返回 cap<int32>，赋给 cap<int8> 变量时需 bitcast）。
                // null 是 typeless 常量，无需 bitcast（`store i32* null` 直接合法）。
                const castReg = this.nextReg();
                this.emit(`${castReg} = bitcast ${rhs.type} ${rhs.val} to ${llvmType}`);
                storedVal = castReg;
            }
            else if (llvmType.endsWith('*') && !rhs.type.endsWith('*') && rhs.val !== 'null') {
                const castReg = this.nextReg();
                this.emit(`${castReg} = inttoptr ${rhs.type} ${rhs.val} to ${llvmType}`);
                storedVal = castReg;
            }
            else if (llvmType !== rhs.type) {
                // 整数收窄/宽化 + 浮点↔整型（`int32 x = 1.5 * 2.0` 需要 fptosi，
                // 否则生成 `store i32 %doubleReg` 的非法 IR）
                storedVal = this.coerceForStore(llvmType, rhs, this.isSignedType(stmt.varType));
            }

            this.emit(`store ${llvmType} ${storedVal}, ${llvmType}* ${ptrReg}`);
            this.setSymbol(stmt.identifier, { ptr: ptrReg, type: llvmType, signed: this.isSignedType(stmt.varType) });
        }
        else if (stmt.type === 'AssignmentStatement') {
            // 指针解引用赋值：*p = v
            if (stmt.target && stmt.target.type === 'Dereference') {
                const ptr = this.visitExpression(stmt.target.target);
                const pointee = ptr.type.endsWith('*') ? ptr.type.slice(0, -1) : 'i32';
                const rhs = this.visitExpression(stmt.value);
                this.untrackTemporary(rhs.val);
                // 类型收窄：*p 的元素类型（如 cap<int8> 的 i8）与右值类型（i32）
                // 不匹配时插入 trunc，否则会生成 `store i8 %i32reg` 的非法 IR。
                const storedVal = this.coerceForStore(pointee, rhs, true);
                this.emit(`store ${pointee} ${storedVal}, ${ptr.type} ${ptr.val}`);
                return;
            }
            if (stmt.target && stmt.target.type === 'IndexExpression') {
                const obj = this.visitExpression((stmt.target as any).object);
                const indices = (stmt.target as any).indices;
                const idx0 = indices.length > 0 ? this.visitExpression(indices[0]).val : '0';
                const idx1 = indices.length > 1 ? this.visitExpression(indices[1]).val : '0';
                const rhs = this.visitExpression(stmt.value);
                this.untrackTemporary(rhs.val);
                this.emit(`call void @rvdl_lattice_set(${obj.type} ${obj.val}, i32 ${idx0}, i32 ${idx1}, i32 ${rhs.val})`);
                return;
            }
            if (stmt.target) {
                const obj = this.visitExpression((stmt.target as any).object);
                const bareType = obj.type.replace(/^%/, '').replace(/\*$/, '');
                const formName = this.formTypeToName.get(bareType);
                if (!formName) throw new Error(`IR Error: Cannot assign field of non-form type '${obj.type}'`);
                const prop = (stmt.target as any).property;
                const field = this.getFormFields(formName).find(f => f.name === prop);
                if (!field) throw new Error(`IR Error: form '${formName}' has no field '${prop}'`);
                const rhs = this.visitExpression(stmt.value);
                this.untrackTemporary(rhs.val);
                this.consumeIfLinear(stmt.value); // move 消费线性字段（如 form 里的 QObject）
                const gep = this.nextReg();
                this.emit(`${gep} = getelementptr %${bareType}, ${obj.type} ${obj.val}, i32 0, i32 ${field.index}`);
                // 位域写：读-改-写（load + 清位 + 移位 or + store）
                if (field.bitWidth !== undefined) {
                    const old = this.nextReg();
                    this.emit(`${old} = load ${field.llvmType}, ${field.llvmType}* ${gep}`);
                    const mask = ((1n << BigInt(field.bitWidth)) - 1n).toString();
                    const clearMask = (((1n << BigInt(field.bitWidth)) - 1n) << BigInt(field.bitOffset ?? 0)).toString();
                    const cleared = this.nextReg();
                    // ~(mask << offset) 的补码：用全 1 与取反。这里直接计算「清除掩码」的取反值按位与。
                    const notMask = (~BigInt(clearMask)) & ((1n << BigInt(this.typeBitWidth(field.llvmType))) - 1n);
                    this.emit(`${cleared} = and ${field.llvmType} ${old}, ${notMask.toString()}`);
                    const maskedVal = this.nextReg();
                    this.emit(`${maskedVal} = and ${field.llvmType} ${rhs.val}, ${mask}`);
                    const shiftedVal = this.nextReg();
                    this.emit(`${shiftedVal} = shl ${field.llvmType} ${maskedVal}, ${field.bitOffset ?? 0}`);
                    const merged = this.nextReg();
                    this.emit(`${merged} = or ${field.llvmType} ${cleared}, ${shiftedVal}`);
                    this.emit(`store ${field.llvmType} ${merged}, ${field.llvmType}* ${gep}`);
                    return;
                }
                this.emit(`store ${field.llvmType} ${rhs.val}, ${field.llvmType}* ${gep}`);
                return;
            }
            const rhs = this.visitExpression(stmt.value);
            this.untrackTemporary(rhs.val);
            this.consumeIfLinear(stmt.value); // move 消费线性变量（如 x = q）
            const sym = this.getSymbol(stmt.name);
            if (sym) {
                const val = this.coerceForStore(sym.type, rhs, sym.signed !== false);
                this.emit(`store ${sym.type} ${val}, ${sym.type}* ${sym.ptr}`);
                this.rebindSymbol(stmt.name); // 重新绑定：清除已消费标记
                return;
            }
            // 全局变量赋值：counter = v → store v, @counter
            if (this.globalVars.has(stmt.name)) {
                const decl = this.globalVars.get(stmt.name)!;
                if (decl.isFixed) throw new Error(`IR Error: Cannot reassign fixed constant '${stmt.name}'`);
                const llvmType = this.getLLVMType(decl.varType);
                // 类型适配：int → cap 需 inttoptr；整数收窄/宽化需 trunc/zext
                let storedVal = rhs.val;
                if (llvmType !== rhs.type && llvmType.endsWith('*') && !rhs.type.endsWith('*')) {
                    const castReg = this.nextReg();
                    this.emit(`${castReg} = inttoptr ${rhs.type} ${rhs.val} to ${llvmType}`);
                    storedVal = castReg;
                } else if (llvmType !== rhs.type) {
                    storedVal = this.coerceForStore(llvmType, rhs, this.isSignedType(decl.varType));
                }
                this.emit(`store ${llvmType} ${storedVal}, ${llvmType}* @${stmt.name}`);
                return;
            }
            throw new Error(`IR Error: Cannot reassign undeclared variable '${stmt.name}'`);
        }
        else if (stmt.type === 'ReturnStatement') {
            if ((stmt as any).isVoid) {
                this.emitCleanup();
                this.emit(`ret void`);
                this.isBlockTerminated = true;
                return;
            }
            const expr = this.visitExpression(stmt.argument);
            this.untrackTemporary(expr.val);
            // move 返回一个线性资源变量（%QObject* / %Qubit*）：所有权转移给调用方，
            // 标记已消费，避免 emitCleanup 在 ret 前重复释放。
            this.consumeIfLinear(stmt.argument);
            this.emitCleanup();
            this.emit(`ret ${expr.type} ${expr.val}`);
            this.isBlockTerminated = true;
        }
        else if (stmt.type === 'WhileStatement') {
            const condLabel = this.nextLabel('while_cond_');
            const bodyLabel = this.nextLabel('while_body_');
            const afterLabel = this.nextLabel('while_after_');
            const hasElse = !!stmt.elseBody && stmt.elseBody.length > 0;
            const elseLabel = hasElse ? this.nextLabel('while_else_') : afterLabel;

            this.loopStack.push({ breakLabel: afterLabel, continueLabel: condLabel });

            if (!this.isBlockTerminated) {
                this.emit(`br label %${condLabel}`);
            }

            this.output.push(`\n${condLabel}:`);
            this.isBlockTerminated = false;
            const cond = this.visitExpression(stmt.condition);
            const condVal = this.toI1(cond);
            this.emit(`br i1 ${condVal}, label %${bodyLabel}, label %${elseLabel}`);
            this.isBlockTerminated = true;

            this.output.push(`\n${bodyLabel}:`);
            this.isBlockTerminated = false;
            this.enterScope();
            for (const bodyStmt of stmt.body) {
                this.visitStatement(bodyStmt);
            }
            this.exitScope();
            if (!this.isBlockTerminated) {
                this.emit(`br label %${condLabel}`);
                this.isBlockTerminated = true;
            }

            this.loopStack.pop();

            if (hasElse) {
                this.output.push(`\n${elseLabel}:`);
                this.isBlockTerminated = false;
                this.enterScope();
                for (const elseStmt of stmt.elseBody!) {
                    this.visitStatement(elseStmt);
                }
                this.exitScope();
                if (!this.isBlockTerminated) {
                    this.emit(`br label %${afterLabel}`);
                    this.isBlockTerminated = true;
                }
            }

            this.output.push(`\n${afterLabel}:`);
            this.isBlockTerminated = false;
        }
        else if (stmt.type === 'IfStatement') {
            const thenLabel = this.nextLabel('if_then_');
            const elseLabel = this.nextLabel('if_else_');
            const afterLabel = this.nextLabel('if_after_');
            const hasElse = !!stmt.alternate && stmt.alternate.length > 0;

            // 条件在当前块内求值（无需像 while 那样先跳到 cond 块）
            const cond = this.visitExpression(stmt.condition);
            const condVal = this.toI1(cond);
            this.emit(`br i1 ${condVal}, label %${thenLabel}, label %${hasElse ? elseLabel : afterLabel}`);
            this.isBlockTerminated = true;

            // then 分支
            this.output.push(`\n${thenLabel}:`);
            this.isBlockTerminated = false;
            this.enterScope();
            for (const bodyStmt of stmt.consequent) {
                this.visitStatement(bodyStmt);
            }
            this.exitScope();
            if (!this.isBlockTerminated) {
                this.emit(`br label %${afterLabel}`);
                this.isBlockTerminated = true;
            }

            // else 分支：else if 在 parser 中已表示为「只含一个 IfStatement 的
            // 数组」，递归走本分支即可自然展开，无需特判。
            if (hasElse) {
                this.output.push(`\n${elseLabel}:`);
                this.isBlockTerminated = false;
                this.enterScope();
                for (const elseStmt of stmt.alternate!) {
                    this.visitStatement(elseStmt);
                }
                this.exitScope();
                if (!this.isBlockTerminated) {
                    this.emit(`br label %${afterLabel}`);
                    this.isBlockTerminated = true;
                }
            }

            this.output.push(`\n${afterLabel}:`);
            this.isBlockTerminated = false;
        }
        else if (stmt.type === 'UnsafeBlock') {
            // unsafe 块在 IR 层面是透明包装：危险操作（裸指针 / MMIO / 内联汇编）
            // 由内层语句各自生成，这里按顺序生成内部语句即可。
            for (const s of stmt.body) {
                this.visitStatement(s);
            }
        }
        else if (stmt.type === 'RouteStatement') {
            const disc = this.visitExpression(stmt.discriminant);
            const caseLabels = stmt.cases.map(() => this.nextLabel('route_path_'));
            const cmpLabels = stmt.cases.map(() => this.nextLabel('route_cmp_'));
            const fallbackLabel = this.nextLabel('route_fallback_');
            const afterLabel = this.nextLabel('route_after_');
            const hasFallback = !!stmt.fallback;

            // 进入第一个比较块
            this.emit(`br label %${cmpLabels[0]}`);
            this.isBlockTerminated = true;

            // 比较链（icmp eq 逐个路径值）
            for (let i = 0; i < stmt.cases.length; i++) {
                this.output.push(`\n${cmpLabels[i]}:`);
                this.isBlockTerminated = false;
                const caseVal = this.visitExpression(stmt.cases[i].value);
                const cmpReg = this.nextReg();
                this.emit(`${cmpReg} = icmp eq ${disc.type} ${disc.val}, ${caseVal.val}`);
                const nextTarget = (i + 1 < stmt.cases.length)
                    ? cmpLabels[i + 1]
                    : (hasFallback ? fallbackLabel : afterLabel);
                this.emit(`br i1 ${cmpReg}, label %${caseLabels[i]}, label %${nextTarget}`);
                this.isBlockTerminated = true;
            }

            // 各路径体
            for (let i = 0; i < stmt.cases.length; i++) {
                this.output.push(`\n${caseLabels[i]}:`);
                this.isBlockTerminated = false;
                this.enterScope();
                for (const s of stmt.cases[i].body) {
                    this.visitStatement(s);
                }
                this.exitScope();
                if (!this.isBlockTerminated) {
                    this.emit(`br label %${afterLabel}`);
                    this.isBlockTerminated = true;
                }
            }

            // fallback（default）
            if (hasFallback) {
                this.output.push(`\n${fallbackLabel}:`);
                this.isBlockTerminated = false;
                this.enterScope();
                for (const s of stmt.fallback!) {
                    this.visitStatement(s);
                }
                this.exitScope();
                if (!this.isBlockTerminated) {
                    this.emit(`br label %${afterLabel}`);
                    this.isBlockTerminated = true;
                }
            }

            this.output.push(`\n${afterLabel}:`);
            this.isBlockTerminated = false;
        }
        else if (stmt.type === 'SpinStatement') {
            const bodyLabel = this.nextLabel('spin_body_');
            const condLabel = this.nextLabel('spin_cond_');
            const afterLabel = this.nextLabel('spin_after_');

            // 先执行循环体
            this.emit(`br label %${bodyLabel}`);
            this.isBlockTerminated = true;

            this.output.push(`\n${bodyLabel}:`);
            this.isBlockTerminated = false;
            this.enterScope();
            for (const s of stmt.body) {
                this.visitStatement(s);
            }
            this.exitScope();
            if (!this.isBlockTerminated) {
                this.emit(`br label %${condLabel}`);
                this.isBlockTerminated = true;
            }

            // 再判断条件
            this.output.push(`\n${condLabel}:`);
            this.isBlockTerminated = false;
            const cond = this.visitExpression(stmt.condition);
            const condVal = this.toI1(cond);
            this.emit(`br i1 ${condVal}, label %${bodyLabel}, label %${afterLabel}`);
            this.isBlockTerminated = true;

            this.output.push(`\n${afterLabel}:`);
            this.isBlockTerminated = false;
        }
        else if (stmt.type === 'SpawnStatement') {
            // 并发线程：spawn 块编译为独立线程函数 @rvdl_thread_N(i8* %env)，经 rvdl_spawn 内建
            // 以 std::thread 启动（detach）。捕获的外层局部变量经 env 闭包结构传递。
            const captured = this.collectFreeVariables(stmt.body, []);
            const capTypes = captured.map(c => this.getSymbol(c)?.type ?? 'i32');
            const threadName = 'rvdl_thread_' + (this.threadCount++);
            const envTypeName = 'env.' + threadName;

            if (capTypes.length > 0) {
                this.typeDefs.push(`%${envTypeName} = type { ${capTypes.join(', ')} }`);
            }
            this.generateThreadFunction(threadName, stmt.body, captured, capTypes);

            if (captured.length === 0) {
                this.emit(`call void @rvdl_spawn(i8* bitcast (void (i8*)* @${threadName} to i8*), i8* null)`);
            } else {
                // malloc env 结构 + 填充捕获变量
                const totalSize = capTypes.reduce((s, t) => s + this.typeSize(t), 0);
                const mallocRes = this.nextReg();
                this.emit(`${mallocRes} = call i8* @rvdl_gc_alloc(i64 ${totalSize})`);
                const envPtr = this.nextReg();
                this.emit(`${envPtr} = bitcast i8* ${mallocRes} to %${envTypeName}*`);
                captured.forEach((capName, i) => {
                    const sym = this.getSymbol(capName);
                    if (!sym) return;
                    const capVal = this.nextReg();
                    this.emit(`${capVal} = load ${capTypes[i]}, ${capTypes[i]}* ${sym.ptr}`);
                    const fieldPtr = this.nextReg();
                    this.emit(`${fieldPtr} = getelementptr %${envTypeName}, %${envTypeName}* ${envPtr}, i32 0, i32 ${i}`);
                    this.emit(`store ${capTypes[i]} ${capVal}, ${capTypes[i]}* ${fieldPtr}`);
                });
                const resultPtr = this.nextReg();
                this.emit(`${resultPtr} = bitcast %${envTypeName}* ${envPtr} to i8*`);
                this.emit(`call void @rvdl_spawn(i8* bitcast (void (i8*)* @${threadName} to i8*), i8* ${resultPtr})`);
            }
        }
        else if (stmt.type === 'EntangleStatement') {
            // 纠缠声明：纯静态信息（供竞争检测），不生成运行时指令。
            this.emit(`; entangle (static entanglement declaration)`);
        }
        else if (stmt.type === 'AsmStatement') {
            // 裸汇编块（多指令序列）：作为 sideeffect 内联汇编发射。
            const tpl = (stmt as any).template.replace(/"/g, '\\"').replace(/\n/g, '\\0A');
            this.emit(`call void asm sideeffect "${tpl}", ""()`);
        }
        else if (stmt.type === 'ForStatement') {
            const condLabel = this.nextLabel('for_cond_');
            const bodyLabel = this.nextLabel('for_body_');
            const updateLabel = this.nextLabel('for_update_');
            const afterLabel = this.nextLabel('for_after_');

            this.enterScope();
            if (stmt.init) {
                this.visitStatement(stmt.init);
            }

            this.loopStack.push({ breakLabel: afterLabel, continueLabel: updateLabel });

            if (!this.isBlockTerminated) {
                this.emit(`br label %${condLabel}`);
            }

            this.output.push(`\n${condLabel}:`);
            this.isBlockTerminated = false;
            if (stmt.condition) {
                const cond = this.visitExpression(stmt.condition);
                const condVal = this.toI1(cond);
                this.emit(`br i1 ${condVal}, label %${bodyLabel}, label %${afterLabel}`);
            } else {
                this.emit(`br label %${bodyLabel}`);
            }
            this.isBlockTerminated = true;

            this.output.push(`\n${bodyLabel}:`);
            this.isBlockTerminated = false;
            // for 循环体是独立作用域：每次迭代 enterScope/exitScope，退出时 cleanup
            // 释放循环体内 new/move 的 %QObject*（对齐 WhileStatement 的行为）。否则
            // 循环体内 `auto psi = new QuantumRegister(2); psi = chaos_drift(psi, dt)`
            // 每次迭代重新绑定的旧 QObject 只在循环结束后 release 一次，导致 qubit
            // 跨迭代泄漏（8 次迭代泄漏 7 个 QObject）。
            this.enterScope();
            for (const bodyStmt of stmt.body) {
                this.visitStatement(bodyStmt);
            }
            this.exitScope();
            if (!this.isBlockTerminated) {
                this.emit(`br label %${updateLabel}`);
                this.isBlockTerminated = true;
            }

            this.output.push(`\n${updateLabel}:`);
            this.isBlockTerminated = false;
            if (stmt.update) {
                this.visitStatement(stmt.update);
            }
            if (!this.isBlockTerminated) {
                this.emit(`br label %${condLabel}`);
                this.isBlockTerminated = true;
            }

            this.loopStack.pop();

            this.output.push(`\n${afterLabel}:`);
            this.isBlockTerminated = false;

            this.exitScope();
        }
        else if (stmt.type === 'BreakStatement') {
            if (this.loopStack.length === 0) {
                throw new Error(`IR Error: 'break' outside of loop`);
            }
            const target = this.loopStack[this.loopStack.length - 1].breakLabel;
            this.emit(`br label %${target}`);
            this.isBlockTerminated = true;
        }
        else if (stmt.type === 'ContinueStatement') {
            if (this.loopStack.length === 0) {
                throw new Error(`IR Error: 'continue' outside of loop`);
            }
            const target = this.loopStack[this.loopStack.length - 1].continueLabel;
            this.emit(`br label %${target}`);
            this.isBlockTerminated = true;
        }
        else if (stmt.type === 'ExpressionStatement') {
            this.visitExpression(stmt.expression);
        }
    }

    private visitExpression(expr: Expression): LLVMValue {
        if (expr.type === 'BinaryExpression') {
            let left = this.visitExpression(expr.left);
            let right = this.visitExpression(expr.right);
            // 整数类型提升：int8/int16 → int32（网络字节操作等，避免 `i8 << i32`
            // 这类操作数类型不匹配的非法 IR）。指针不提升。
            if (!left.type.endsWith('*') && !right.type.endsWith('*')) {
                const promote = (v: LLVMValue): LLVMValue => {
                    if (v.type === 'i8' || v.type === 'i16') {
                        const t = this.nextReg();
                        // 无符号（uint8/uint16）零扩展，有符号（int8/int16）符号扩展
                        this.emit(`${t} = ${v.signed === false ? 'zext' : 'sext'} ${v.type} ${v.val} to i32`);
                        return { val: t, type: 'i32', signed: v.signed };
                    }
                    return v;
                };
                left = promote(left);
                right = promote(right);
            }
            // 数值类型统一：int 与 double/float 混合运算时，把整数提升为浮点
            // （如 `0.1 * i` 里 i 是 i32，需 sitofp 转 double 再 fmul，否则类型不匹配）。
            if (left.type !== right.type && !left.type.endsWith('*') && !right.type.endsWith('*')) {
                const isFloat = (t: string) => t === 'double' || t === 'float';
                const isInt = (t: string) => t.startsWith('i');
                if (isFloat(left.type) && isInt(right.type)) {
                    const t = this.nextReg();
                    this.emit(`${t} = ${right.signed === false ? 'uitofp' : 'sitofp'} ${right.type} ${right.val} to ${left.type}`);
                    right = { val: t, type: left.type, signed: right.signed };
                } else if (isInt(left.type) && isFloat(right.type)) {
                    const t = this.nextReg();
                    this.emit(`${t} = ${left.signed === false ? 'uitofp' : 'sitofp'} ${left.type} ${left.val} to ${right.type}`);
                    left = { val: t, type: right.type, signed: left.signed };
                }
            }
            // 复数运算（complex64 / complex128，量子态矢量）：extractvalue + 分量运算 + insertvalue
            const isComplex = left.type === right.type &&
                (left.type === '{ float, float }' || left.type === '{ double, double }');
            if (isComplex) {
                const elem = left.type === '{ float, float }' ? 'float' : 'double';
                const lre = this.nextReg(); this.emit(`${lre} = extractvalue ${left.type} ${left.val}, 0`);
                const lim = this.nextReg(); this.emit(`${lim} = extractvalue ${left.type} ${left.val}, 1`);
                const rre = this.nextReg(); this.emit(`${rre} = extractvalue ${right.type} ${right.val}, 0`);
                const rim = this.nextReg(); this.emit(`${rim} = extractvalue ${right.type} ${right.val}, 1`);
                let re: string, im: string;
                if (expr.operator === '+') {
                    re = this.nextReg(); this.emit(`${re} = fadd ${elem} ${lre}, ${rre}`);
                    im = this.nextReg(); this.emit(`${im} = fadd ${elem} ${lim}, ${rim}`);
                } else if (expr.operator === '-') {
                    re = this.nextReg(); this.emit(`${re} = fsub ${elem} ${lre}, ${rre}`);
                    im = this.nextReg(); this.emit(`${im} = fsub ${elem} ${lim}, ${rim}`);
                } else if (expr.operator === '*') {
                    const a = this.nextReg(); this.emit(`${a} = fmul ${elem} ${lre}, ${rre}`);
                    const b = this.nextReg(); this.emit(`${b} = fmul ${elem} ${lim}, ${rim}`);
                    re = this.nextReg(); this.emit(`${re} = fsub ${elem} ${a}, ${b}`);
                    const c = this.nextReg(); this.emit(`${c} = fmul ${elem} ${lre}, ${rim}`);
                    const d = this.nextReg(); this.emit(`${d} = fmul ${elem} ${lim}, ${rre}`);
                    im = this.nextReg(); this.emit(`${im} = fadd ${elem} ${c}, ${d}`);
                } else {
                    throw new Error(`IR Error: Unsupported complex operator '${expr.operator}'`);
                }
                const res0 = this.nextReg();
                this.emit(`${res0} = insertvalue ${left.type} undef, ${elem} ${re}, 0`);
                const res1 = this.nextReg();
                this.emit(`${res1} = insertvalue ${left.type} ${res0}, ${elem} ${im}, 1`);
                return { val: res1, type: left.type };
            }
            const resReg = this.nextReg();
            const isFloat = left.type === 'double' || left.type === 'float';
            if (isFloat) {
                // 混合运算时整数常量已被 sitofp 成 %reg；若某一侧仍残留裸整数字面量
                // （如常量折叠路径），改写为带小数点的浮点字面量，避免
                // `fdiv double %x, 360` 被 LLVM 判为「integer/byte constant」。
                if (/^-?\d+$/.test(left.val)) left = { ...left, val: this.formatFloatLiteral(Number(left.val)) };
                if (/^-?\d+$/.test(right.val)) right = { ...right, val: this.formatFloatLiteral(Number(right.val)) };
            }
            // 整型字面量与整数变量混合运算时，字面量必须跟随另一侧的位宽。
            //
            // 缺陷背景（实测）：`int64 q = ...; h0 = h0 + 19 * q;` 会生成
            //   %x = mul i32 19, %q        // q 是 i64 —— LLVM 类型不匹配
            // 变量声明 `int64 x = 19;` 走 coerceForStore 所以没事，
            // 但表达式里的裸字面量没有任何地方对齐类型。
            if (!isFloat && !left.type.endsWith('*') && !right.type.endsWith('*')) {
                const lw = left.type, rw = right.type;
                const isLit = (v: LLVMValue) => /^-?\d+$/.test(v.val);
                if (lw !== rw && isLit(left) && !isLit(right)) {
                    left = { ...left, type: rw };
                } else if (rw !== lw && isLit(right) && !isLit(left)) {
                    right = { ...right, type: lw };
                }
            }
            // 无符号整数（uint64 等）：除法/取模/右移/比较改用无符号指令
            const unsigned = left.signed === false;

            if (expr.operator === '+') {
                if (left.type.endsWith('*')) {
                    // 指针偏移：p + i -> getelementptr（cap<T> 的指针算术）
                    const elem = left.type.slice(0, -1);
                    this.emit(`${resReg} = getelementptr ${elem}, ${left.type} ${left.val}, i32 ${right.val}`);
                    return { val: resReg, type: left.type };
                }
                this.emit(`${resReg} = ${isFloat ? 'fadd' : 'add'} ${left.type} ${left.val}, ${right.val}`);
                return { val: resReg, type: left.type, signed: left.signed };
            }
            else if (expr.operator === '-') {
                this.emit(`${resReg} = ${isFloat ? 'fsub' : 'sub'} ${left.type} ${left.val}, ${right.val}`);
                return { val: resReg, type: left.type, signed: left.signed };
            }
            else if (expr.operator === '*') {
                this.emit(`${resReg} = ${isFloat ? 'fmul' : 'mul'} ${left.type} ${left.val}, ${right.val}`);
                return { val: resReg, type: left.type, signed: left.signed };
            }
            else if (expr.operator === '/') {
                const op = isFloat ? 'fdiv' : (unsigned ? 'udiv' : 'sdiv');
                this.emit(`${resReg} = ${op} ${left.type} ${left.val}, ${right.val}`);
                return { val: resReg, type: left.type, signed: left.signed };
            }
            else if (expr.operator === '%') {
                const op = isFloat ? 'frem' : (unsigned ? 'urem' : 'srem');
                this.emit(`${resReg} = ${op} ${left.type} ${left.val}, ${right.val}`);
                return { val: resReg, type: left.type, signed: left.signed };
            }
            else if (expr.operator === '&') {
                this.emit(`${resReg} = and ${left.type} ${left.val}, ${right.val}`);
                return { val: resReg, type: left.type, signed: left.signed };
            }
            else if (expr.operator === '|') {
                this.emit(`${resReg} = or ${left.type} ${left.val}, ${right.val}`);
                return { val: resReg, type: left.type, signed: left.signed };
            }
            else if (expr.operator === '^') {
                this.emit(`${resReg} = xor ${left.type} ${left.val}, ${right.val}`);
                return { val: resReg, type: left.type, signed: left.signed };
            }
            else if (expr.operator === '<<') {
                this.emit(`${resReg} = shl ${left.type} ${left.val}, ${right.val}`);
                return { val: resReg, type: left.type, signed: left.signed };
            }
            else if (expr.operator === '>>') {
                this.emit(`${resReg} = ${unsigned ? 'lshr' : 'ashr'} ${left.type} ${left.val}, ${right.val}`);
                return { val: resReg, type: left.type, signed: left.signed };
            }
            else if (expr.operator === '<' || expr.operator === '>' ||
                     expr.operator === '<=' || expr.operator === '>=' ||
                     expr.operator === '==' || expr.operator === '!=') {
                const cmpMap: Record<string, string> = isFloat
                    ? { '<': 'olt', '>': 'ogt', '<=': 'ole', '>=': 'oge', '==': 'oeq', '!=': 'one' }
                    : unsigned
                        ? { '<': 'ult', '>': 'ugt', '<=': 'ule', '>=': 'uge', '==': 'eq', '!=': 'ne' }
                        : { '<': 'slt', '>': 'sgt', '<=': 'sle', '>=': 'sge', '==': 'eq', '!=': 'ne' };
                const instr = isFloat ? 'fcmp' : 'icmp';
                this.emit(`${resReg} = ${instr} ${cmpMap[expr.operator]} ${left.type} ${left.val}, ${right.val}`);
                return { val: resReg, type: 'i1' };
            }
            throw new Error(`IR Error: Unsupported operator '${expr.operator}'`);
        }

        if (expr.type === 'LogicalExpression') {
            // 短路结果用命名寄存器（避免 splice 到 entry 后与未命名编号冲突）
            const resPtr = '%logic_ptr_' + (this.labelCount++);
            this.allocas.push(` ${resPtr} = alloca i1`);
            const endLabel = this.nextLabel('logic_end_');
            const shortLabel = this.nextLabel('logic_short_');
            const evalLabel = this.nextLabel('logic_eval_');

            const left = this.visitExpression(expr.left);
            const leftCond = this.toI1(left);

            if (expr.operator === '&&') {
                this.emit(`br i1 ${leftCond}, label %${evalLabel}, label %${shortLabel}`);
            } else {
                this.emit(`br i1 ${leftCond}, label %${shortLabel}, label %${evalLabel}`);
            }

            this.output.push(`\n${shortLabel}:`);
            this.emit(`store i1 ${expr.operator === '&&' ? 'false' : 'true'}, i1* ${resPtr}`);
            this.emit(`br label %${endLabel}`);

            this.output.push(`\n${evalLabel}:`);
            const right = this.visitExpression(expr.right);
            const rightCond = this.toI1(right);
            this.emit(`store i1 ${rightCond}, i1* ${resPtr}`);
            this.emit(`br label %${endLabel}`);

            this.output.push(`\n${endLabel}:`);
            const resReg = this.nextReg();
            this.emit(`${resReg} = load i1, i1* ${resPtr}`);
            return { val: resReg, type: 'i1' };
        }

        if (expr.type === 'UnaryExpression') {
            const arg = this.visitExpression(expr.argument);
            if (expr.operator === '!') {
                const argCond = this.toI1(arg);
                const resReg = this.nextReg();
                this.emit(`${resReg} = xor i1 ${argCond}, true`);
                return { val: resReg, type: 'i1' };
            } else if ((expr.operator as string) === '~') {
                // 按位取反：xor -1
                const resReg = this.nextReg();
                this.emit(`${resReg} = xor ${arg.type} ${arg.val}, -1`);
                return { val: resReg, type: arg.type };
            } else {
                const resReg = this.nextReg();
                if (arg.type === 'double' || arg.type === 'float') {
                    this.emit(`${resReg} = fsub ${arg.type} -0.0, ${arg.val}`);
                } else {
                    this.emit(`${resReg} = sub ${arg.type} 0, ${arg.val}`);
                }
                return { val: resReg, type: arg.type };
            }
        }

        if (expr.type === 'FunctionExpression') {
            const lambdaName = 'lambda_' + (this.lambdaCount++);
            const retTypeStr = expr.returnType ?? 'int32';
            const retType = this.getLLVMType(retTypeStr);
            const paramTypes = expr.params.map(p => this.getLLVMType(p.type));

            // 收集自由变量（同样得捕获的外部变量）
            const captured = this.collectFreeVariables(expr.body, expr.params);
            const capTypes = captured.map(c => this.getSymbol(c)?.type ?? 'i32');

            // 定义闭包 struct 类型：{ i8* fn_ptr, cap_types... }
            const closureTypeName = 'closure.' + lambdaName;
            const capFields = capTypes.join(', ');
            this.typeDefs.push(`%${closureTypeName} = type { ${capFields ? 'i8*, ' + capFields : 'i8*'} }`);

            // 生成 thunk（含 env + 捕获变量读取）
            this.generateLambdaFunction(lambdaName, expr.params, retTypeStr, expr.body, captured, capTypes);

            // malloc 闭包对象
            const totalSize = 8 + capTypes.reduce((s, t) => s + this.typeSize(t), 0);
            const mallocRes = this.nextReg();
            this.emit(`${mallocRes} = call i8* @rvdl_gc_alloc(i64 ${totalSize})`);
            const closurePtr = this.nextReg();
            this.emit(`${closurePtr} = bitcast i8* ${mallocRes} to %${closureTypeName}*`);

            // store fn_ptr（field 0）
            const fnPtrField = this.nextReg();
            this.emit(`${fnPtrField} = getelementptr %${closureTypeName}, %${closureTypeName}* ${closurePtr}, i32 0, i32 0`);
            const thunkPtr = this.nextReg();
            this.emit(`${thunkPtr} = bitcast ${retType} (i8*, ${paramTypes.join(', ')})* @${lambdaName} to i8*`);
            this.emit(`store i8* ${thunkPtr}, i8** ${fnPtrField}`);

            // store 捕获变量（field 1..n）
            captured.forEach((capName, i) => {
                const sym = this.getSymbol(capName);
                if (!sym) return;
                const capVal = this.nextReg();
                this.emit(`${capVal} = load ${capTypes[i]}, ${capTypes[i]}* ${sym.ptr}`);
                const fieldPtr = this.nextReg();
                this.emit(`${fieldPtr} = getelementptr %${closureTypeName}, %${closureTypeName}* ${closurePtr}, i32 0, i32 ${i + 1}`);
                this.emit(`store ${capTypes[i]} ${capVal}, ${capTypes[i]}* ${fieldPtr}`);
            });

            // 返回闭包对象指针（i8*）
            const resultPtr = this.nextReg();
            this.emit(`${resultPtr} = bitcast %${closureTypeName}* ${closurePtr} to i8*`);
            return { val: resultPtr, type: 'i8*' };
        }

        if (expr.type === 'NumberLiteral') {
            if (expr.isFloat) {
                // 整数值浮点字面量（如 0.0 / 1024.0）需显式保留小数点，
                // 否则 LLVM 会把 `double 0` 判为整数常量而报错；
                // 极小值（1e-9）JS 会输出 `1e-9`（同样无小数点），一并处理。
                return { val: this.formatFloatLiteral(expr.value), type: 'double' };
            }
            // 大整数（> 2^53，JS Number 精度丢失）：保留原始字面量（48 位 LBA / 64 位字段）
            const v = (expr.raw && !Number.isSafeInteger(expr.value)) ? expr.raw : expr.value.toString();
            return { val: v, type: 'i32' };
        }

        if (expr.type === 'StringLiteral') {
            const strGlobal = this.addStringLiteral(expr.value);
            const strLen = Buffer.byteLength(expr.value, 'utf8') + 1;
            const resReg = this.nextReg();
            this.emit(`${resReg} = getelementptr inbounds [${strLen} x i8], [${strLen} x i8]* ${strGlobal}, i64 0, i64 0`);
            return { val: resReg, type: 'i8*' };
        }

        if (expr.type === 'NullLiteral') {
            return { val: 'null', type: 'i8*' };
        }

        if (expr.type === 'Dereference') {
            const ptr = this.visitExpression(expr.target);
            const pointee = ptr.type.endsWith('*') ? ptr.type.slice(0, -1) : 'i32';
            const resReg = this.nextReg();
            this.emit(`${resReg} = load ${pointee}, ${ptr.type} ${ptr.val}`);
            return { val: resReg, type: pointee };
        }

        if (expr.type === 'AddressOf') {
            if (expr.target.type === 'Identifier') {
                // &x（局部变量）→ 其 alloca 地址（type*）；&fn（函数名）→ 函数指针类型
                const sym = this.getSymbol(expr.target.name);
                if (sym) {
                    return { val: sym.ptr, type: sym.type + '*' };
                }
                // 全局变量取地址：&counter → @counter（cap<T>）
                if (this.globalVars.has(expr.target.name)) {
                    const decl = this.globalVars.get(expr.target.name)!;
                    const llvmType = this.getLLVMType(decl.varType);
                    return { val: '@' + expr.target.name, type: llvmType + '*' };
                }
                // 函数名取址：返回真正的函数指针类型（ret (params)*），供 fn<...> 变量 / 间接调用
                const userFn = this.userFunctions.get(expr.target.name);
                if (userFn) {
                    const ret = this.getLLVMType(userFn.returnType);
                    const params = userFn.params.map(p => this.getLLVMType(p.type));
                    return { val: '@' + expr.target.name, type: `${ret} (${params.join(', ')})*` };
                }
                const extSig = this.externSigs.get(expr.target.name);
                if (extSig) {
                    return { val: '@' + expr.target.name, type: `${extSig.ret} (${extSig.params.join(', ')})*` };
                }
                return { val: '@' + expr.target.name, type: 'i8*' };  // 函数符号
            }
            throw new Error('IR Error: address-of requires a function name');
        }

        if (expr.type === 'FuseExpression') {
            const resultTy = 'i32';
            // 先分配结果临时变量，再求值判别式——保证寄存器编号在最终 IR 中单调递增
            // （allocas 会被 splice 到函数入口，命名寄存器避免与未命名编号冲突）。
            const resultPtr = '%fuse_ptr_' + (this.labelCount++);
            this.allocas.push(` ${resultPtr} = alloca ${resultTy}`);

            const disc = this.visitExpression(expr.discriminant);

            const armLabels = expr.arms.map(() => this.nextLabel('fuse_arm_'));
            const cmpLabels = expr.arms.map(() => this.nextLabel('fuse_cmp_'));
            const afterLabel = this.nextLabel('fuse_after_');

            this.emit(`br label %${cmpLabels[0]}`);
            this.isBlockTerminated = true;

            // 比较链
            for (let i = 0; i < expr.arms.length; i++) {
                const arm = expr.arms[i];
                this.output.push(`\n${cmpLabels[i]}:`);
                this.isBlockTerminated = false;
                if (arm.pattern === null) {
                    this.emit(`br label %${armLabels[i]}`);
                    this.isBlockTerminated = true;
                } else {
                    const pat = this.visitExpression(arm.pattern);
                    const cmpReg = this.nextReg();
                    this.emit(`${cmpReg} = icmp eq ${disc.type} ${disc.val}, ${pat.val}`);
                    const nextTarget = (i + 1 < expr.arms.length) ? cmpLabels[i + 1] : afterLabel;
                    this.emit(`br i1 ${cmpReg}, label %${armLabels[i]}, label %${nextTarget}`);
                    this.isBlockTerminated = true;
                }
            }

            // 各 arm 求值 + 存储结果
            for (let i = 0; i < expr.arms.length; i++) {
                this.output.push(`\n${armLabels[i]}:`);
                this.isBlockTerminated = false;
                const val = this.visitExpression(expr.arms[i].value);
                this.emit(`store ${resultTy} ${val.val}, ${resultTy}* ${resultPtr}`);
                this.emit(`br label %${afterLabel}`);
                this.isBlockTerminated = true;
            }

            // 汇聚：加载结果
            this.output.push(`\n${afterLabel}:`);
            this.isBlockTerminated = false;
            const resReg = this.nextReg();
            this.emit(`${resReg} = load ${resultTy}, ${resultTy}* ${resultPtr}`);
            return { val: resReg, type: resultTy };
        }

        if (expr.type === 'NativeExpression') {
            if (expr.operands && expr.operands.length > 0) {
                const ops = expr.operands.map(o => this.visitExpression(o));
                const constraints = ops.map(() => 'r').join(',');
                const argTypes = ops.map(o => o.type).join(', ');
                const argVals = ops.map(o => o.val).join(', ');
                this.emit(`call void asm sideeffect "${expr.template}", "${constraints}"(${argTypes} ${argVals})`);
            } else {
                this.emit(`call void asm sideeffect "${expr.template}", ""()`);
            }
            return { val: 'void', type: 'void' };
        }

        if (expr.type === 'Identifier') {
            if (this.flavorMembers.has(expr.name)) {
                // 味成员：直接内联整数值
                return { val: String(this.flavorMembers.get(expr.name)), type: 'i32' };
            }
            if (this.globalVars.has(expr.name)) {
                // 全局变量：从 @name load
                const decl = this.globalVars.get(expr.name)!;
                const llvmType = this.getLLVMType(decl.varType);
                const loadReg = this.nextReg();
                this.emit(`${loadReg} = load ${llvmType}, ${llvmType}* @${expr.name}`);
                return { val: loadReg, type: llvmType, signed: this.isSignedType(decl.varType) };
            }
            const sym = this.getSymbol(expr.name);
            if (!sym) throw new Error(`IR Error: Unresolved variable '${expr.name}'`);
            const loadReg = this.nextReg();
            this.emit(`${loadReg} = load ${sym.type}, ${sym.type}* ${sym.ptr}`);
            return { val: loadReg, type: sym.type, signed: sym.signed };
        }

        if (expr.type === 'NewExpression') {
            if (expr.className.startsWith('lattice<')) {
                const inner = expr.className.slice('lattice<'.length, -1);
                const parts = inner.split(',').map((s: string) => s.trim());
                const boundary = parts.length > 1 ? this.boundaryCode(parts[1]) : 0;
                const rank = expr.arguments.length;
                const arg0 = rank > 0 ? this.visitExpression(expr.arguments[0]).val : '1';
                const arg1 = rank > 1 ? this.visitExpression(expr.arguments[1]).val : '1';
                const lresReg = this.nextReg();
                this.emit(`${lresReg} = call %Lattice* @rvdl_lattice_new(i32 ${rank}, i32 ${arg0}, i32 ${arg1}, i32 ${boundary})`);
                this.trackTemporary(lresReg, '%Lattice*');
                return { val: lresReg, type: '%Lattice*' };
            }
            if (expr.className === 'BellState') {
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QObject* @rvdl_create_BellState()`);
                this.trackTemporary(resReg, '%QObject*');
                return { val: resReg, type: '%QObject*' };
            }
            if (expr.className === 'DiracState') {
                const arg = expr.arguments.length > 0 ? this.visitExpression(expr.arguments[0]).val : '0';
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QObject* @rvdl_create_DiracState(i32 ${arg})`);
                this.trackTemporary(resReg, '%QObject*');
                return { val: resReg, type: '%QObject*' };
            }
            if (expr.className === 'QuantumRegister') {
                const arg = this.visitExpression(expr.arguments[0]).val;
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QObject* @rvdl_create_QuantumRegister(i32 ${arg})`);
                this.trackTemporary(resReg, '%QObject*');
                return { val: resReg, type: '%QObject*' };
            }

            let actualName = expr.className.split('<')[0];
            if (expr.className.includes('<')) {
                const inst = this.instantiateTemplate(expr.className);
                if (inst) actualName = inst;
            }

            if (this.forms.has(actualName)) {
                const formType = '%' + this.mangleForm(actualName);
                const objReg = '%form_obj_' + (this.regCount++);
                if (expr.heapAlloc) {
                    const sizeBytes = this.formSizeBytes(actualName);
                    const raw = this.nextReg();
                    this.emit(`${raw} = call i8* @rvdl_gc_alloc(i64 ${sizeBytes})`);
                    this.emit(`${objReg} = bitcast i8* ${raw} to ${formType}*`);
                } else {
                    this.allocas.push(` ${objReg} = alloca ${formType}`);
                }
                let vtType: string | null = null;
                for (const [traitName, impls] of this.traitImpls) {
                    if (impls.has(actualName)) { vtType = '%' + this.mangleForm(traitName) + '.vtable'; break; }
                }
                if (vtType) {
                    const vtPtr = this.nextReg();
                    this.emit(`${vtPtr} = getelementptr ${formType}, ${formType}* ${objReg}, i32 0, i32 0`);
                    this.emit(`store i8* bitcast (${vtType}* @${this.mangleForm(actualName)}.vtable to i8*), i8** ${vtPtr}`);
                }
                return { val: objReg, type: formType + '*' };
            }
        }

        if (expr.type === 'FunctionCall') {
            if (expr.name === 'alloc') {
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %Qubit* @__quantum__rt__qubit_allocate()`);
                this.trackTemporary(resReg, '%Qubit*');
                return { val: resReg, type: '%Qubit*' };
            }

            // 任意基构建量子态：basis_state(theta, phi, value)
            if (expr.name === 'basis_state') {
                const thetaArg = this.visitExpression(expr.arguments[0]);
                const phiArg = this.visitExpression(expr.arguments[1]);
                const valueArg = this.visitExpression(expr.arguments[2]);
                const thetaVal = thetaArg.type === 'i32' ? `${thetaArg.val}.0` : thetaArg.val;
                const phiVal = phiArg.type === 'i32' ? `${phiArg.val}.0` : phiArg.val;
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QObject* @rvdl_create_basis_state(double ${thetaVal}, double ${phiVal}, i32 ${valueArg.val})`);
                this.trackTemporary(resReg, '%QObject*');
                return { val: resReg, type: '%QObject*' };
            }

            // 晶格内省：lattice_rank / lattice_size / lattice_boundary
            if (expr.name === 'lattice_rank') {
                const obj = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_lattice_rank(${obj.type} ${obj.val})`);
                return { val: resReg, type: 'i32' };
            }
            if (expr.name === 'lattice_size') {
                const obj = this.visitExpression(expr.arguments[0]);
                const dim = this.visitExpression(expr.arguments[1]).val;
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_lattice_size(${obj.type} ${obj.val}, i32 ${dim})`);
                return { val: resReg, type: 'i32' };
            }
            if (expr.name === 'lattice_boundary') {
                const obj = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_lattice_boundary(${obj.type} ${obj.val})`);
                return { val: resReg, type: 'i32' };
            }

            // 经典 GUI（cgui_*）
            if (expr.name === 'cgui_init') {
                const w = this.visitExpression(expr.arguments[0]);
                const h = this.visitExpression(expr.arguments[1]);
                const t = this.visitExpression(expr.arguments[2]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_cgui_init(i32 ${w.val}, i32 ${h.val}, i8* ${t.val})`);
                return { val: resReg, type: 'i32' };
            }
            if (expr.name === 'cgui_should_close') {
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_cgui_should_close()`);
                return { val: resReg, type: 'i32' };
            }
            if (expr.name === 'cgui_begin_frame') {
                this.emit(`call void @rvdl_cgui_begin_frame()`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'cgui_end_frame') {
                this.emit(`call void @rvdl_cgui_end_frame()`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'cgui_button') {
                const l = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_cgui_button(i8* ${l.val})`);
                return { val: resReg, type: 'i32' };
            }
            if (expr.name === 'cgui_text') {
                const t = this.visitExpression(expr.arguments[0]);
                this.emit(`call void @rvdl_cgui_text(i8* ${t.val})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'cgui_text_int') {
                const v = this.visitExpression(expr.arguments[0]);
                this.emit(`call void @rvdl_cgui_text_int(i32 ${v.val})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'cgui_beep') {
                const f = this.visitExpression(expr.arguments[0]);
                const d = this.visitExpression(expr.arguments[1]);
                this.emit(`call void @rvdl_cgui_beep(i32 ${f.val}, i32 ${d.val})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'cgui_width') {
                const r = this.nextReg();
                this.emit(`${r} = call i32 @rvdl_cgui_width()`);
                return { val: r, type: 'i32' };
            }
            if (expr.name === 'cgui_height') {
                const r = this.nextReg();
                this.emit(`${r} = call i32 @rvdl_cgui_height()`);
                return { val: r, type: 'i32' };
            }
            if (expr.name === 'cgui_panel') {
                const x = this.visitExpression(expr.arguments[0]);
                const y = this.visitExpression(expr.arguments[1]);
                const w = this.visitExpression(expr.arguments[2]);
                const h = this.visitExpression(expr.arguments[3]);
                const t = this.visitExpression(expr.arguments[4]);
                this.emit(`call void @rvdl_cgui_panel(i32 ${x.val}, i32 ${y.val}, i32 ${w.val}, i32 ${h.val}, i8* ${t.val})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'cgui_panel_end') {
                this.emit(`call void @rvdl_cgui_panel_end()`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'cgui_row') {
                const c = this.visitExpression(expr.arguments[0]);
                const h = this.visitExpression(expr.arguments[1]);
                this.emit(`call void @rvdl_cgui_row(i32 ${c.val}, i32 ${h.val})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'cgui_mouse_x') {
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_cgui_mouse_x()`);
                return { val: resReg, type: 'i32' };
            }
            if (expr.name === 'cgui_mouse_y') {
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_cgui_mouse_y()`);
                return { val: resReg, type: 'i32' };
            }
            if (expr.name === 'cgui_mouse_left_clicked') {
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_cgui_mouse_left_clicked()`);
                return { val: resReg, type: 'i32' };
            }

            // 经典图形引擎（cgfx_*）
            if (expr.name === 'cgfx_rect') {
                const a = expr.arguments.map(x => this.visitExpression(x).val);
                this.emit(`call void @rvdl_cgfx_rect(${a.map(v => `i32 ${v}`).join(', ')})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'cgfx_line') {
                const a = expr.arguments.map(x => this.visitExpression(x).val);
                this.emit(`call void @rvdl_cgfx_line(${a.map(v => `i32 ${v}`).join(', ')})`);
                return { val: 'void', type: 'void' };
            }
            // 折线：首参是 arr<i32, N>（IR 里是 [N x i32]*），需 decay 成
            // i32*（GEP 到元素 0）才能传给 C ABI；cap<int32> 则本就是 i32*。
            // 曲线/图表用它而不是循环 cgfx_line —— 接缝会错位（见 ClassicGfx.hpp）。
            if (expr.name === 'cgfx_polyline' || expr.name === 'cgfx_polyline_a') {
                const suffix = expr.name === 'cgfx_polyline_a' ? '_a' : '';
                const args = expr.arguments.map(x => this.visitExpression(x));
                let ptr = 'null';
                if (args.length > 0 && args[0].val !== 'null') {
                    if (args[0].type.startsWith('[')) {
                        // [N x i32]* → i32*
                        const g = this.nextReg();
                        this.emit(`${g} = getelementptr ${args[0].type}, ${args[0].type}* ${args[0].val}, i32 0, i32 0`);
                        ptr = g;
                    } else {
                        ptr = args[0].val;
                    }
                }
                const rest = args.slice(1).map(v => `i32 ${v.val}`).join(', ');
                this.emit(`call void @rvdl_cgfx_polyline${suffix}(i32* ${ptr}${rest ? ', ' + rest : ''})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'cgfx_ellipse') {
                const a = expr.arguments.map(x => this.visitExpression(x).val);
                this.emit(`call void @rvdl_cgfx_ellipse(${a.map(v => `i32 ${v}`).join(', ')})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'cgfx_triangle') {
                const a = expr.arguments.map(x => this.visitExpression(x).val);
                this.emit(`call void @rvdl_cgfx_triangle(${a.map(v => `i32 ${v}`).join(', ')})`);
                return { val: 'void', type: 'void' };
            }

            // 带 alpha 的经典图形原语（粒子特效淡出）
            if (expr.name === 'cgfx_rect_a' || expr.name === 'cgfx_line_a' ||
                expr.name === 'cgfx_ellipse_a' || expr.name === 'cgfx_triangle_a') {
                const a = expr.arguments.map(x => this.visitExpression(x).val);
                this.emit(`call void @rvdl_cgfx_${expr.name.slice(5)}(${a.map(v => `i32 ${v}`).join(', ')})`);
                return { val: 'void', type: 'void' };
            }
            // 折线的 alpha 变体：首参同样是指针，不能走上面的全 i32 泛化分支。
            if (expr.name === 'rvdl_sys_call') {
                const args = expr.arguments.map(a => this.visitExpression(a));
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_sys_call(${args.map(a => `i32 ${a.val}`).join(', ')})`);
                return { val: resReg, type: 'i32' };
            }
            if (expr.name === 'rvdl_sys_calld') {
                const args = expr.arguments.map(a => this.visitExpression(a));
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_sys_calld(i32 ${args[0].val}, double ${args[1].val}, double ${args[2].val})`);
                return { val: resReg, type: 'double' };
            }
            if (expr.name === 'rvdl_sys_log') {
                const level = this.visitExpression(expr.arguments[0]);
                const msg = this.visitExpression(expr.arguments[1]);
                this.emit(`call void @rvdl_sys_log(i32 ${level.val}, i8* ${msg.val})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'rvdl_sys_logi') {
                const level = this.visitExpression(expr.arguments[0]);
                const v = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_sys_logi(i32 ${level.val}, i32 ${v.val})`);
                return { val: resReg, type: 'i32' };
            }

            // QMS 数值内核
            if (expr.name === 'rvdl_qms_gap') {
                const args = expr.arguments.map(a => this.visitExpression(a));
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_qms_gap(i32 ${args[0].val}, double ${args[1].val}, double ${args[2].val})`);
                return { val: resReg, type: 'double' };
            }
            if (expr.name === 'rvdl_mix_bound') {
                const args = expr.arguments.map(a => this.visitExpression(a));
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_mix_bound(double ${args[0].val}, double ${args[1].val}, double ${args[2].val})`);
                return { val: resReg, type: 'double' };
            }
            if (expr.name === 'rvdl_qms_conc') {
                const args = expr.arguments.map(a => this.visitExpression(a));
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_qms_conc(double ${args[0].val}, double ${args[1].val})`);
                return { val: resReg, type: 'double' };
            }

            // 同步内存事件（系统级：自旋锁 / 计数器；语义源自弱内存模型）
            if (expr.name === 'sync_load') {
                const p = this.visitExpression(expr.arguments[0]);
                const pointee = p.type.endsWith('*') ? p.type.slice(0, -1) : 'i32';
                const resReg = this.nextReg();
                this.emit(`${resReg} = load atomic ${pointee}, ${p.type} ${p.val} seq_cst, align 4`);
                return { val: resReg, type: pointee };
            }
            if (expr.name === 'sync_store') {
                const p = this.visitExpression(expr.arguments[0]);
                const v = this.visitExpression(expr.arguments[1]);
                const pointee = p.type.endsWith('*') ? p.type.slice(0, -1) : 'i32';
                this.emit(`store atomic ${pointee} ${v.val}, ${p.type} ${p.val} seq_cst, align 4`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'sync_add') {
                const p = this.visitExpression(expr.arguments[0]);
                const v = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = atomicrmw add ${p.type} ${p.val}, i32 ${v.val} seq_cst, align 4`);
                return { val: resReg, type: 'i32' };
            }
            if (expr.name === 'sync_cas') {
                const p = this.visitExpression(expr.arguments[0]);
                const oldV = this.visitExpression(expr.arguments[1]);
                const newV = this.visitExpression(expr.arguments[2]);
                const pointee = p.type.endsWith('*') ? p.type.slice(0, -1) : 'i32';
                const tmpReg = this.nextReg();
                const resReg = this.nextReg();
                this.emit(`${tmpReg} = cmpxchg ${p.type} ${p.val}, ${pointee} ${oldV.val}, ${pointee} ${newV.val} seq_cst seq_cst, align 4`);
                this.emit(`${resReg} = extractvalue { ${pointee}, i1 } ${tmpReg}, 0`);
                return { val: resReg, type: pointee };
            }
            // 锁原语（Q-Digest 锁集的运行时来源）：
            //   sync_lock   = 原子 TAS（atomicrmw xchg 0 -> 1），返回旧值，供 while 自旋等待；
            //   sync_unlock = 原子释放（store atomic 0）。
            if (expr.name === 'sync_lock') {
                const p = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = atomicrmw xchg ${p.type} ${p.val}, i32 1 seq_cst, align 4`);
                return { val: resReg, type: 'i32' };  // 旧值：0 成功获取，1 已被持有
            }
            if (expr.name === 'sync_unlock') {
                const p = this.visitExpression(expr.arguments[0]);
                const pointee = p.type.endsWith('*') ? p.type.slice(0, -1) : 'i32';
                this.emit(`store atomic ${pointee} 0, ${p.type} ${p.val} seq_cst, align 4`);
                return { val: 'void', type: 'void' };
            }

            // 端口 I/O（x86 内联汇编：outb/inb，值在 AL、端口在 DX）
            // 内联汇编要求端口为 i16、值为 i8，而 rvdl 整型默认 i32：
            // 字面量直接拼写即可；变量（%N）需先 trunc 到目标宽度，
            // 否则出现 `i8 %5` 而 %5 实为 i32 的类型不匹配。
            if (expr.name === 'outb') {
                const port = this.visitExpression(expr.arguments[0]);
                const value = this.visitExpression(expr.arguments[1]);
                let portVal = port.val;
                if (port.val.startsWith('%')) {
                    const t = this.nextReg();
                    this.emit(`${t} = trunc ${port.type} ${port.val} to i16`);
                    portVal = t;
                }
                let valueVal = value.val;
                if (value.val.startsWith('%')) {
                    const t = this.nextReg();
                    this.emit(`${t} = trunc ${value.type} ${value.val} to i8`);
                    valueVal = t;
                }
                const tpl = 'outb ${0:b}, ${1:w}';
                this.emit(`call void asm sideeffect "${tpl}", "{ax},{dx},~{dirflag},~{fpsr},~{flags}"(i8 ${valueVal}, i16 ${portVal})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'inb') {
                const port = this.visitExpression(expr.arguments[0]);
                let portVal = port.val;
                if (port.val.startsWith('%')) {
                    const t = this.nextReg();
                    this.emit(`${t} = trunc ${port.type} ${port.val} to i16`);
                    portVal = t;
                }
                const tmpReg = this.nextReg();
                const resReg = this.nextReg();
                const tpl = 'inb ${1:w}, ${0:b}';
                this.emit(`${tmpReg} = call i8 asm sideeffect "${tpl}", "={ax},{dx},~{dirflag},~{fpsr},~{flags}"(i16 ${portVal})`);
                this.emit(`${resReg} = zext i8 ${tmpReg} to i32`);
                return { val: resReg, type: 'i32' };
            }
            // 16 位端口 I/O（PCI/ATA 底层）：outw(port, value) / inw(port)
            if (expr.name === 'outw') {
                const port = this.visitExpression(expr.arguments[0]);
                const value = this.visitExpression(expr.arguments[1]);
                let portVal = port.val;
                if (port.val.startsWith('%')) {
                    const t = this.nextReg();
                    this.emit(`${t} = trunc ${port.type} ${port.val} to i16`);
                    portVal = t;
                }
                let valueVal = value.val;
                if (value.val.startsWith('%')) {
                    const t = this.nextReg();
                    this.emit(`${t} = trunc ${value.type} ${value.val} to i16`);
                    valueVal = t;
                }
                const tpl = 'outw ${0:w}, ${1:w}';
                this.emit(`call void asm sideeffect "${tpl}", "{ax},{dx},~{dirflag},~{fpsr},~{flags}"(i16 ${valueVal}, i16 ${portVal})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'inw') {
                const port = this.visitExpression(expr.arguments[0]);
                let portVal = port.val;
                if (port.val.startsWith('%')) {
                    const t = this.nextReg();
                    this.emit(`${t} = trunc ${port.type} ${port.val} to i16`);
                    portVal = t;
                }
                const tmpReg = this.nextReg();
                const resReg = this.nextReg();
                const tpl = 'inw ${1:w}, ${0:w}';
                this.emit(`${tmpReg} = call i16 asm sideeffect "${tpl}", "={ax},{dx},~{dirflag},~{fpsr},~{flags}"(i16 ${portVal})`);
                this.emit(`${resReg} = zext i16 ${tmpReg} to i32`);
                return { val: resReg, type: 'i32' };
            }
            // 32 位端口 I/O（PCI 配置空间等）：outl(port, value) / inl(port)
            if (expr.name === 'outl') {
                const port = this.visitExpression(expr.arguments[0]);
                const value = this.visitExpression(expr.arguments[1]);
                let portVal = port.val;
                if (port.val.startsWith('%')) {
                    const t = this.nextReg();
                    this.emit(`${t} = trunc ${port.type} ${port.val} to i16`);
                    portVal = t;
                }
                const tpl = 'outl ${0:k}, ${1:w}';
                this.emit(`call void asm sideeffect "${tpl}", "{eax},{dx},~{dirflag},~{fpsr},~{flags}"(i32 ${value.val}, i16 ${portVal})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'inl') {
                const port = this.visitExpression(expr.arguments[0]);
                let portVal = port.val;
                if (port.val.startsWith('%')) {
                    const t = this.nextReg();
                    this.emit(`${t} = trunc ${port.type} ${port.val} to i16`);
                    portVal = t;
                }
                const tmpReg = this.nextReg();
                const tpl = 'inl ${1:w}, ${0:k}';
                this.emit(`${tmpReg} = call i32 asm sideeffect "${tpl}", "={eax},{dx},~{dirflag},~{fpsr},~{flags}"(i16 ${portVal})`);
                return { val: tmpReg, type: 'i32' };
            }
            if (expr.name === 'rvdl_gc_alloc') {
                const size = this.visitExpression(expr.arguments[0]);
                const zReg = this.nextReg();
                const rawReg = this.nextReg();
                const resReg = this.nextReg();
                this.emit(`${zReg} = zext i32 ${size.val} to i64`);
                this.emit(`${rawReg} = call i8* @rvdl_gc_alloc(i64 ${zReg})`);
                // 以 int32 字为单位看待原始内存
                this.emit(`${resReg} = bitcast i8* ${rawReg} to i32*`);
                return { val: resReg, type: 'i32*' };
            }
            if (expr.name === 'rvdl_sys_callp') {
                const args = expr.arguments.map(a => this.visitExpression(a));
                const z0 = this.nextReg(), z1 = this.nextReg(), z2 = this.nextReg();
                this.emit(`${z0} = zext i32 ${args[1].val} to i64`);
                this.emit(`${z1} = zext i32 ${args[2].val} to i64`);
                this.emit(`${z2} = zext i32 ${args[3].val} to i64`);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i8* @rvdl_sys_callp(i32 ${args[0].val}, i64 ${z0}, i64 ${z1}, i64 ${z2})`);
                return { val: resReg, type: 'i8*' };
            }
            if (expr.name === 'rvdl_gc_free') {
                const p = this.visitExpression(expr.arguments[0]);
                const bc = this.nextReg();
                this.emit(`${bc} = bitcast ${p.type} ${p.val} to i8*`);
                this.emit(`call void @rvdl_gc_free(i8* ${bc})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'strlen') {
                // 字符串遍历：strlen(s) 返回字节长度
                const s = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @strlen(i8* ${s.val})`);
                return { val: resReg, type: 'i32' };
            }
            if (expr.name === 'kglobals_addr') {
                // 内核全局状态区基址（shim.c 提供固定落点，rvdl 经 inttoptr 读写）
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @kglobals_addr()`);
                return { val: resReg, type: 'i32' };
            }
            if (expr.name === 'addr') {
                const p = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = ptrtoint ${p.type} ${p.val} to i32`);
                return { val: resReg, type: 'i32' };
            }

            // ─── volatile 内存访问（MMIO 轮询不被 -O2 优化为死循环）────────────────
            if (expr.name === 'volatile_load') {
                const p = this.visitExpression(expr.arguments[0]);
                const pointee = p.type.endsWith('*') ? p.type.slice(0, -1) : 'i32';
                const resReg = this.nextReg();
                this.emit(`${resReg} = load volatile ${pointee}, ${p.type} ${p.val}`);
                return { val: resReg, type: pointee };
            }
            if (expr.name === 'volatile_store') {
                const p = this.visitExpression(expr.arguments[0]);
                const v = this.visitExpression(expr.arguments[1]);
                const pointee = p.type.endsWith('*') ? p.type.slice(0, -1) : 'i32';
                this.emit(`store volatile ${pointee} ${v.val}, ${p.type} ${p.val}`);
                return { val: 'void', type: 'void' };
            }

            // ─── 读寄存器 / MSR / CPUID（x86 架构边界：寄存器读回 rvdl 变量）─────────
            if (expr.name === 'read_cr0' || expr.name === 'read_cr2' ||
                expr.name === 'read_cr3' || expr.name === 'read_cr4') {
                const cr = expr.name.replace('read_cr', 'cr');
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i64 asm sideeffect "mov \${0}, %${cr}", "=r,~{dirflag},~{fpsr},~{flags}"()`);
                return { val: resReg, type: 'i64' };
            }
            if (expr.name === 'write_cr0' || expr.name === 'write_cr3') {
                const v = this.visitExpression(expr.arguments[0]);
                const cr = expr.name.replace('write_cr', 'cr');
                this.emit(`call void asm sideeffect "mov %${cr}, \${0}", "r,~{memory},~{dirflag},~{fpsr},~{flags}"(i64 ${v.val})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'invlpg') {
                const a = this.visitExpression(expr.arguments[0]);
                this.emit(`call void asm sideeffect "invlpg (\${0})", "r,~{memory},~{dirflag},~{fpsr},~{flags}"(i64 ${a.val})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'rdmsr') {
                const msr = this.visitExpression(expr.arguments[0]);
                return { val: this.emitEdxEaxAsm('rdmsr', '={eax},={edx},{ecx},~{memory},~{dirflag},~{fpsr},~{flags}', [{ type: 'i32', val: msr.val }]), type: 'i64' };
            }
            if (expr.name === 'wrmsr') {
                const msr = this.visitExpression(expr.arguments[0]);
                const v = this.visitExpression(expr.arguments[1]);
                const lo = this.nextReg(), hi = this.nextReg();
                this.emit(`${lo} = trunc i64 ${v.val} to i32`);
                this.emit(`${hi} = lshr i64 ${v.val}, 32`);
                const hi32 = this.nextReg();
                this.emit(`${hi32} = trunc i64 ${hi} to i32`);
                this.emit(`call void asm sideeffect "wrmsr", "{ecx},{eax},{edx},~{memory},~{dirflag},~{fpsr},~{flags}"(i32 ${msr.val}, i32 ${lo}, i32 ${hi32})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'cpuid') {
                const leaf = this.visitExpression(expr.arguments[0]);
                const subleaf = this.visitExpression(expr.arguments[1]);
                const eaxPtr = this.visitExpression(expr.arguments[2]);
                const ebxPtr = this.visitExpression(expr.arguments[3]);
                const ecxPtr = this.visitExpression(expr.arguments[4]);
                const edxPtr = this.visitExpression(expr.arguments[5]);
                const res = this.nextReg();
                this.emit(`${res} = call { i32, i32, i32, i32 } asm sideeffect "cpuid", "={eax},={ebx},={ecx},={edx},{eax},{ecx},~{memory},~{dirflag},~{fpsr},~{flags}"(i32 ${leaf.val}, i32 ${subleaf.val})`);
                const r0 = this.nextReg(), r1 = this.nextReg(), r2 = this.nextReg(), r3 = this.nextReg();
                this.emit(`${r0} = extractvalue { i32, i32, i32, i32 } ${res}, 0`);
                this.emit(`${r1} = extractvalue { i32, i32, i32, i32 } ${res}, 1`);
                this.emit(`${r2} = extractvalue { i32, i32, i32, i32 } ${res}, 2`);
                this.emit(`${r3} = extractvalue { i32, i32, i32, i32 } ${res}, 3`);
                this.emit(`store i32 ${r0}, i32* ${eaxPtr.val}`);
                this.emit(`store i32 ${r1}, i32* ${ebxPtr.val}`);
                this.emit(`store i32 ${r2}, i32* ${ecxPtr.val}`);
                this.emit(`store i32 ${r3}, i32* ${edxPtr.val}`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'rdtsc') {
                return { val: this.emitEdxEaxAsm('rdtsc', '={eax},={edx},~{dirflag},~{fpsr},~{flags}', []), type: 'i64' };
            }
            if (expr.name === 'read_rflags') {
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i64 asm sideeffect "pushfq; pop \${0}", "=r,~{dirflag},~{fpsr},~{flags}"()`);
                return { val: resReg, type: 'i64' };
            }
            if (expr.name === 'xgetbv') {
                const xcr = this.visitExpression(expr.arguments[0]);
                return { val: this.emitEdxEaxAsm('xgetbv', '={eax},={edx},{ecx},~{memory},~{dirflag},~{fpsr},~{flags}', [{ type: 'i32', val: xcr.val }]), type: 'i64' };
            }

            // ─── 复数（complex64/complex128，量子态矢量）────────────────────
            if (expr.name === 'complex') {
                const re = this.visitExpression(expr.arguments[0]);
                const im = this.visitExpression(expr.arguments[1]);
                const cty = '{ double, double }';
                const res0 = this.nextReg();
                this.emit(`${res0} = insertvalue ${cty} undef, double ${re.val}, 0`);
                const res1 = this.nextReg();
                this.emit(`${res1} = insertvalue ${cty} ${res0}, double ${im.val}, 1`);
                return { val: res1, type: cty };
            }
            if (expr.name === 'real' || expr.name === 'imag' || expr.name === 'conj' || expr.name === 'cabs') {
                const z = this.visitExpression(expr.arguments[0]);
                const isC64 = z.type === '{ float, float }';
                const elem = isC64 ? 'float' : 'double';
                if (expr.name === 'real' || expr.name === 'imag') {
                    const idx = expr.name === 'real' ? 0 : 1;
                    const res = this.nextReg();
                    this.emit(`${res} = extractvalue ${z.type} ${z.val}, ${idx}`);
                    return { val: res, type: elem };
                }
                if (expr.name === 'conj') {
                    const re = this.nextReg(); this.emit(`${re} = extractvalue ${z.type} ${z.val}, 0`);
                    const im = this.nextReg(); this.emit(`${im} = extractvalue ${z.type} ${z.val}, 1`);
                    const neg = this.nextReg(); this.emit(`${neg} = fneg ${elem} ${im}`);
                    const res0 = this.nextReg();
                    this.emit(`${res0} = insertvalue ${z.type} undef, ${elem} ${re}, 0`);
                    const res1 = this.nextReg();
                    this.emit(`${res1} = insertvalue ${z.type} ${res0}, ${elem} ${neg}, 1`);
                    return { val: res1, type: z.type };
                }
                // cabs：|z| = sqrt(re^2 + im^2)
                const re = this.nextReg(); this.emit(`${re} = extractvalue ${z.type} ${z.val}, 0`);
                const im = this.nextReg(); this.emit(`${im} = extractvalue ${z.type} ${z.val}, 1`);
                const a = this.nextReg(); this.emit(`${a} = fmul ${elem} ${re}, ${re}`);
                const b = this.nextReg(); this.emit(`${b} = fmul ${elem} ${im}, ${im}`);
                const s = this.nextReg(); this.emit(`${s} = fadd ${elem} ${a}, ${b}`);
                const res = this.nextReg();
                this.emit(`${res} = call ${elem} @llvm.sqrt.f${isC64 ? '32' : '64'}(${elem} ${s})`);
                return { val: res, type: elem };
            }

            if (expr.name === 'measure') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @__quantum__qis__measure_int(${arg.type} ${arg.val})`);
                return { val: resReg, type: 'i32' };
            }

            if (expr.name === 'h' || expr.name === 'x' || expr.name === 'y' ||
                expr.name === 'z' || expr.name === 's' || expr.name === 't') {
                const arg = this.visitExpression(expr.arguments[0]);
                this.emit(`call void @__quantum__qis__${expr.name}(${arg.type} ${arg.val})`);
                this.emitNoiseIfNeeded(arg.type, arg.val);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'rz' || expr.name === 'rx' || expr.name === 'ry') {
                const q = this.visitExpression(expr.arguments[0]);
                const angle = this.visitExpression(expr.arguments[1]);
                const angleVal = angle.type === 'i32' ? `${angle.val}.0` : angle.val;
                this.emit(`call void @__quantum__qis__${expr.name}(double ${angleVal}, ${q.type} ${q.val})`);
                this.emitNoiseIfNeeded(q.type, q.val);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'cnot' || expr.name === 'swap' || expr.name === 'braid') {
                const a = this.visitExpression(expr.arguments[0]);
                const b = this.visitExpression(expr.arguments[1]);
                this.emit(`call void @__quantum__qis__${expr.name}(${a.type} ${a.val}, ${b.type} ${b.val})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'toffoli') {
                const c1 = this.visitExpression(expr.arguments[0]);
                const c2 = this.visitExpression(expr.arguments[1]);
                const t = this.visitExpression(expr.arguments[2]);
                this.emit(`call void @__quantum__qis__toffoli(${c1.type} ${c1.val}, ${c2.type} ${c2.val}, ${t.type} ${t.val})`);
                return { val: 'void', type: 'void' };
            }

            // 受控门（可逆编织 @[steer] 的运行时目标）
            if (expr.name === 'cx' || expr.name === 'ch') {
                const c = this.visitExpression(expr.arguments[0]);
                const t = this.visitExpression(expr.arguments[1]);
                this.emit(`call void @__quantum__qis__${expr.name}(${c.type} ${c.val}, ${t.type} ${t.val})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'crz') {
                const c = this.visitExpression(expr.arguments[0]);
                const t = this.visitExpression(expr.arguments[1]);
                const angle = this.visitExpression(expr.arguments[2]);
                const angleVal = angle.type === 'i32' ? `${angle.val}.0` : angle.val;
                this.emit(`call void @__quantum__qis__crz(${c.type} ${c.val}, ${t.type} ${t.val}, double ${angleVal})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'cswap') {
                const c = this.visitExpression(expr.arguments[0]);
                const a = this.visitExpression(expr.arguments[1]);
                const b = this.visitExpression(expr.arguments[2]);
                this.emit(`call void @__quantum__qis__cswap(${c.type} ${c.val}, ${a.type} ${a.val}, ${b.type} ${b.val})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'c_toffoli') {
                const c = this.visitExpression(expr.arguments[0]);
                const a = this.visitExpression(expr.arguments[1]);
                const b = this.visitExpression(expr.arguments[2]);
                const t = this.visitExpression(expr.arguments[3]);
                this.emit(`call void @__quantum__qis__c_toffoli(${c.type} ${c.val}, ${a.type} ${a.val}, ${b.type} ${b.val}, ${t.type} ${t.val})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'cbraid') {
                const c = this.visitExpression(expr.arguments[0]);
                const a = this.visitExpression(expr.arguments[1]);
                const b = this.visitExpression(expr.arguments[2]);
                this.emit(`call void @__quantum__qis__cbraid(${c.type} ${c.val}, ${a.type} ${a.val}, ${b.type} ${b.val})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'qft' || expr.name === 'iqft') {
                const n = this.visitExpression(expr.arguments[0]);
                this.emit(`call void @__quantum__qis__${expr.name}(i32 ${n.val})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'cqft') {
                const c = this.visitExpression(expr.arguments[0]);
                const n = this.visitExpression(expr.arguments[1]);
                this.emit(`call void @__quantum__qis__cqft(${c.type} ${c.val}, i32 ${n.val})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'measure_x' || expr.name === 'measure_y') {
                const arg = this.visitExpression(expr.arguments[0]);
                const basis = expr.name === 'measure_x' ? 88 : 89;  // 'X' / 'Y'
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @__quantum__qis__measure_basis(${arg.type} ${arg.val}, i8 ${basis})`);
                return { val: resReg, type: 'i32' };
            }

            if (expr.name === 'encode_text') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QObject* @rvdl_encode_text(i8* ${arg.val})`);
                this.trackTemporary(resReg, '%QObject*');
                return { val: resReg, type: '%QObject*' };
            }

            if (expr.name === 'encode_amplitudes') {
                const arg = this.visitExpression(expr.arguments[0]);
                const nArg = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QObject* @rvdl_encode_amplitudes(i8* ${arg.val}, i32 ${nArg.val})`);
                this.trackTemporary(resReg, '%QObject*');
                return { val: resReg, type: '%QObject*' };
            }

            if (expr.name === 'encode_image') {
                const arg = this.visitExpression(expr.arguments[0]);
                const nArg = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QObject* @rvdl_encode_image(i8* ${arg.val}, i32 ${nArg.val})`);
                this.trackTemporary(resReg, '%QObject*');
                return { val: resReg, type: '%QObject*' };
            }

            if (expr.name === 'encode_adaptive') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QObject* @rvdl_encode_adaptive(i8* ${arg.val})`);
                this.trackTemporary(resReg, '%QObject*');
                return { val: resReg, type: '%QObject*' };
            }

            if (expr.name === 'qlm_invoke') {
                const dataArg = this.visitExpression(expr.arguments[0]);
                const epochsArg = this.visitExpression(expr.arguments[1]);
                const lrArg = this.visitExpression(expr.arguments[2]);
                const lrVal = lrArg.type === 'i32' ? `${lrArg.val}.0` : lrArg.val;
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QModel* @rvdl_qlm_invoke(%QObject* ${dataArg.val}, i32 ${epochsArg.val}, double ${lrVal})`);
                return { val: resReg, type: '%QModel*' };
            }

            if (expr.name === 'qlm_load') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QModel* @rvdl_qlm_load(i8* ${arg.val})`);
                return { val: resReg, type: '%QModel*' };
            }

            if (expr.name === 'rvdl_encode_string') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QObject* @rvdl_encode_string(i8* ${arg.val})`);
                this.trackTemporary(resReg, '%QObject*');
                return { val: resReg, type: '%QObject*' };
            }

            if (expr.name === 'qlm_forward') {
                const modelArg = this.visitExpression(expr.arguments[0]);
                const inputArg = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                this.emit(`call void @rvdl_qlm_forward(%QModel* ${modelArg.val}, %QObject* ${inputArg.val})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'rvdl_decode_string') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i8* @rvdl_decode_string(%QObject* ${arg.val})`);
                return { val: resReg, type: 'i8*' };
            }

            if (expr.name === 'mind_read') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QObject* @rvdl_mind_read(i8* ${arg.val})`);
                this.trackTemporary(resReg, '%QObject*');
                return { val: resReg, type: '%QObject*' };
            }

            if (expr.name === 'mind_train') {
                const dataArg = this.visitExpression(expr.arguments[0]);
                const epochsArg = this.visitExpression(expr.arguments[1]);
                const lrArg = this.visitExpression(expr.arguments[2]);
                const lrVal = lrArg.type === 'i32' ? `${lrArg.val}.0` : lrArg.val;
                this.emit(`call void @rvdl_mind_train(%QObject* ${dataArg.val}, i32 ${epochsArg.val}, double ${lrVal})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'mind_feedback') {
                const arg = this.visitExpression(expr.arguments[0]);
                this.emit(`call void @rvdl_mind_feedback(%QObject* ${arg.val})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'veda_qlm_train') {
                const dataArg = this.visitExpression(expr.arguments[0]);
                const epochsArg = this.visitExpression(expr.arguments[1]);
                const lrArg = this.visitExpression(expr.arguments[2]);
                const lrVal = lrArg.type === 'i32' ? `${lrArg.val}.0` : lrArg.val;
                this.emit(`call void @rvdl_veda_qlm_train(%QObject* ${dataArg.val}, i32 ${epochsArg.val}, double ${lrVal})`);
                return { val: 'void', type: 'void' };
            }

            // ─── QRC 量子储备池内置函数 ──────────────────────────
            if (expr.name === 'qrc_new') {
                // 防御：实参个数由语义层校验（Signature Error），但 IR 层必须对
                // 畸形输入保持健壮 —— 否则少参调用会在此抛 TypeError 而中断整个编译。
                const i32zero = { val: '0', type: 'i32' };
                const qubitsArg = expr.arguments[0] ? this.visitExpression(expr.arguments[0]) : i32zero;
                const layersArg = expr.arguments[1] ? this.visitExpression(expr.arguments[1]) : i32zero;
                const resReg = this.nextReg();
                // 三参数形式带显式 out_dim（原先固定 out_dim=1，只够标量演示任务）。
                if (expr.arguments.length >= 3) {
                    const outDimArg = this.visitExpression(expr.arguments[2]);
                    this.emit(`${resReg} = call %QReservoir* @rvdl_qrc_new_ex(i32 ${qubitsArg.val}, i32 ${layersArg.val}, i32 ${outDimArg.val})`);
                } else {
                    this.emit(`${resReg} = call %QReservoir* @rvdl_qrc_new(i32 ${qubitsArg.val}, i32 ${layersArg.val})`);
                }
                return { val: resReg, type: '%QReservoir*' };
            }

            // 用真实训练数据训练（原 qrc_train 只跑合成正弦，仅够自检）。
            if (expr.name === 'qrc_train_ex') {
                // 同上：实参不足时用占位值继续生成，不在 IR 层抛异常。
                const i32zero = { val: '0', type: 'i32' };
                const ptrZero = { val: 'null', type: 'i8*' };
                const pick = (i: number, dflt: LLVMValue): LLVMValue =>
                    (expr.arguments[i] ? this.visitExpression(expr.arguments[i]) : dflt);
                const resArg = pick(0, ptrZero);
                const epochsArg = pick(1, i32zero);
                const lrArg = pick(2, i32zero);
                const inputsArg = pick(3, ptrZero);
                const targetsArg = pick(4, ptrZero);
                const nSamplesArg = pick(5, i32zero);
                const nFeaturesArg = pick(6, i32zero);
                const nOutputsArg = pick(7, i32zero);
                const lrVal = lrArg.type === 'i32' ? `${lrArg.val}.0` : lrArg.val;
                this.emit(`call void @rvdl_qrc_train_ex(%QReservoir* ${resArg.val}, i32 ${epochsArg.val}, double ${lrVal}, double* ${inputsArg.val}, double* ${targetsArg.val}, i32 ${nSamplesArg.val}, i32 ${nFeaturesArg.val}, i32 ${nOutputsArg.val})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'qrc_train') {
                const resArg = this.visitExpression(expr.arguments[0]);
                const epochsArg = this.visitExpression(expr.arguments[1]);
                const lrArg = this.visitExpression(expr.arguments[2]);
                const lrVal = lrArg.type === 'i32' ? `${lrArg.val}.0` : lrArg.val;
                this.emit(`call void @rvdl_qrc_train(%QReservoir* ${resArg.val}, i32 ${epochsArg.val}, double ${lrVal})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'qrc_probe') {
                const resArg = this.visitExpression(expr.arguments[0]);
                const dataArg = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QObject* @rvdl_qrc_probe(%QReservoir* ${resArg.val}, %QObject* ${dataArg.val})`);
                this.trackTemporary(resReg, '%QObject*');
                return { val: resReg, type: '%QObject*' };
            }

            if (expr.name === 'qrc_predict') {
                const resArg = this.visitExpression(expr.arguments[0]);
                const dataArg = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QObject* @rvdl_qrc_predict(%QReservoir* ${resArg.val}, %QObject* ${dataArg.val})`);
                this.trackTemporary(resReg, '%QObject*');
                return { val: resReg, type: '%QObject*' };
            }

            if (expr.name === 'qrc_release') {
                const resArg = this.visitExpression(expr.arguments[0]);
                this.emit(`call void @rvdl_qrc_release(%QReservoir* ${resArg.val})`);
                return { val: 'void', type: 'void' };
            }

            // ─── TQNF 拓扑量子神经场内置函数 ──────────────────────
            if (expr.name === 'dla_dim') {
                const specArg = this.visitExpression(expr.arguments[0]);   // string → i8*
                const nArg = this.visitExpression(expr.arguments[1]);      // int32 → i32
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_dla_dim(i8* ${specArg.val}, i32 ${nArg.val})`);
                return { val: resReg, type: 'i32' };
            }

            if (expr.name === 'qstate_entropy') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_qstate_entropy(%QObject* ${arg.val})`);
                return { val: resReg, type: 'double' };
            }

            if (expr.name === 'qstate_fidelity') {
                const aArg = this.visitExpression(expr.arguments[0]);
                const bArg = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_qstate_fidelity(%QObject* ${aArg.val}, %QObject* ${bArg.val})`);
                return { val: resReg, type: 'double' };
            }

            // ── nqcmath / expermath 语言层桥接（QMathAbi.hpp）──────────────
            if (expr.name === 'qstate_purity' || expr.name === 'qstate_linear_entropy') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_${expr.name}(%QObject* ${arg.val})`);
                return { val: resReg, type: 'double' };
            }

            if (expr.name === 'qstate_renyi_entropy') {
                const objArg = this.visitExpression(expr.arguments[0]);
                const alphaArg = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_qstate_renyi_entropy(%QObject* ${objArg.val}, double ${alphaArg.val})`);
                return { val: resReg, type: 'double' };
            }

            // nqcmath 向量运算：arr<double,N> → double*（GEP 到元素 0）。
            // 与 cgfx_polyline 的首参 decay 同一套 —— 数组在 IR 里是 [N x double]*，
            // 要 GEP 一次才是指向元素 0 的 double*。
            if (expr.name === 'math_dot' || expr.name === 'math_norm') {
                const args = expr.arguments.map(x => this.visitExpression(x));
                const decay = (arg: { val: string; type: string }): string => {
                    if (arg.type.startsWith('[')) {
                        const g = this.nextReg();
                        this.emit(`${g} = getelementptr ${arg.type}, ${arg.type}* ${arg.val}, i32 0, i32 0`);
                        return g;
                    }
                    return arg.val;
                };
                const a = decay(args[0]);
                const resReg = this.nextReg();
                if (expr.name === 'math_dot') {
                    const b = decay(args[1]);
                    this.emit(`${resReg} = call double @rvdl_math_dot(double* ${a}, double* ${b}, i32 ${args[2].val})`);
                } else {
                    this.emit(`${resReg} = call double @rvdl_math_norm(double* ${a}, i32 ${args[1].val})`);
                }
                return { val: resReg, type: 'double' };
            }

            if (expr.name === 'qattention') {
                const qArg = this.visitExpression(expr.arguments[0]);
                const kArg = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_qattention(%QObject* ${qArg.val}, %QObject* ${kArg.val})`);
                return { val: resReg, type: 'double' };
            }

            if (expr.name === 'shannon4') {
                const a0 = this.visitExpression(expr.arguments[0]);
                const a1 = this.visitExpression(expr.arguments[1]);
                const a2 = this.visitExpression(expr.arguments[2]);
                const a3 = this.visitExpression(expr.arguments[3]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_shannon4(i32 ${a0.val}, i32 ${a1.val}, i32 ${a2.val}, i32 ${a3.val})`);
                return { val: resReg, type: 'double' };
            }

            if (expr.name === 'shannon8') {
                const args = expr.arguments.map(a => this.visitExpression(a));
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_shannon8(${args.map(a => `i32 ${a.val}`).join(', ')})`);
                return { val: resReg, type: 'double' };
            }

            // ─── TQNF QObject 层量子门（FFN）──────────────────────
            if (expr.name === 'qgate_h' || expr.name === 'qgate_x') {
                const objArg = this.visitExpression(expr.arguments[0]);
                const idxArg = this.visitExpression(expr.arguments[1]);
                const sym = expr.name === 'qgate_h' ? 'rvdl_qgate_h' : 'rvdl_qgate_x';
                this.emit(`call void @${sym}(%QObject* ${objArg.val}, i32 ${idxArg.val})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'qgate_rz') {
                const objArg = this.visitExpression(expr.arguments[0]);
                const idxArg = this.visitExpression(expr.arguments[1]);
                const angArg = this.visitExpression(expr.arguments[2]);
                const angVal = angArg.type === 'i32' ? `double ${angArg.val}.0` : `double ${angArg.val}`;
                this.emit(`call void @rvdl_qgate_rz(%QObject* ${objArg.val}, i32 ${idxArg.val}, ${angVal})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'qgate_cnot') {
                const objArg = this.visitExpression(expr.arguments[0]);
                const cArg = this.visitExpression(expr.arguments[1]);
                const tArg = this.visitExpression(expr.arguments[2]);
                this.emit(`call void @rvdl_qgate_cnot(%QObject* ${objArg.val}, i32 ${cArg.val}, i32 ${tArg.val})`);
                return { val: 'void', type: 'void' };
            }
            if (expr.name === 'qgate_cnot_pair') {
                const aArg = this.visitExpression(expr.arguments[0]);
                const iaArg = this.visitExpression(expr.arguments[1]);
                const bArg = this.visitExpression(expr.arguments[2]);
                const ibArg = this.visitExpression(expr.arguments[3]);
                this.emit(`call void @rvdl_qgate_cnot_pair(%QObject* ${aArg.val}, i32 ${iaArg.val}, %QObject* ${bArg.val}, i32 ${ibArg.val})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'qexpect_z') {
                const objArg = this.visitExpression(expr.arguments[0]);
                const idxArg = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_qexpect_z(%QObject* ${objArg.val}, i32 ${idxArg.val})`);
                return { val: resReg, type: 'double' };
            }

            if (expr.name === 'qmeasure') {
                const objArg = this.visitExpression(expr.arguments[0]);
                const idxArg = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_qmeasure(%QObject* ${objArg.val}, i32 ${idxArg.val})`);
                return { val: resReg, type: 'i32' };
            }

            if (expr.name === 'qobj_num_qubits') {
                const objArg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_qobj_num_qubits(%QObject* ${objArg.val})`);
                return { val: resReg, type: 'i32' };
            }

            // ─── 量子通道逆因果容量（回程能力）──────────────────────
            if (expr.name === 'retrocausal_imax' || expr.name === 'retrocausal_idoe' ||
                expr.name === 'retrocausal_q_capacity' || expr.name === 'retrocausal_c_capacity' ||
                expr.name === 'retrocausal_gain') {
                const kindArg = this.visitExpression(expr.arguments[0]);
                const pArg = this.visitExpression(expr.arguments[1]);
                const pVal = pArg.type === 'i32' ? `double ${pArg.val}.0` : `double ${pArg.val}`;
                const cname = {
                    retrocausal_imax: 'rvdl_retrocausal_imax',
                    retrocausal_idoe: 'rvdl_retrocausal_idoe',
                    retrocausal_q_capacity: 'rvdl_retrocausal_q_capacity',
                    retrocausal_c_capacity: 'rvdl_retrocausal_c_capacity',
                    retrocausal_gain: 'rvdl_retrocausal_gain',
                }[expr.name]!;
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @${cname}(i32 ${kindArg.val}, ${pVal})`);
                return { val: resReg, type: 'double' };
            }
            if (expr.name === 'retrocausal_q_one_shot' || expr.name === 'retrocausal_deformed') {
                const kindArg = this.visitExpression(expr.arguments[0]);
                const pArg = this.visitExpression(expr.arguments[1]);
                const qArg = this.visitExpression(expr.arguments[2]);
                const pVal = pArg.type === 'i32' ? `double ${pArg.val}.0` : `double ${pArg.val}`;
                const qVal = qArg.type === 'i32' ? `double ${qArg.val}.0` : `double ${qArg.val}`;
                const cname = expr.name === 'retrocausal_q_one_shot' ? 'rvdl_retrocausal_q_one_shot' : 'rvdl_retrocausal_deformed';
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @${cname}(i32 ${kindArg.val}, ${pVal}, ${qVal})`);
                return { val: resReg, type: 'double' };
            }
            if (expr.name === 'retrocausal_ctc_q_capacity' || expr.name === 'retrocausal_ctc_c_capacity' ||
                expr.name === 'retrocausal_ctc_gain') {
                const nArg = this.visitExpression(expr.arguments[0]);
                const thArg = this.visitExpression(expr.arguments[1]);
                const mu2Arg = this.visitExpression(expr.arguments[2]);
                const lamArg = this.visitExpression(expr.arguments[3]);
                const thVal = thArg.type === 'i32' ? `double ${thArg.val}.0` : `double ${thArg.val}`;
                const mu2Val = mu2Arg.type === 'i32' ? `double ${mu2Arg.val}.0` : `double ${mu2Arg.val}`;
                const lamVal = lamArg.type === 'i32' ? `double ${lamArg.val}.0` : `double ${lamArg.val}`;
                const cname = {
                    retrocausal_ctc_q_capacity: 'rvdl_retrocausal_ctc_q_capacity',
                    retrocausal_ctc_c_capacity: 'rvdl_retrocausal_ctc_c_capacity',
                    retrocausal_ctc_gain: 'rvdl_retrocausal_ctc_gain',
                }[expr.name]!;
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @${cname}(i32 ${nArg.val}, ${thVal}, ${mu2Val}, ${lamVal})`);
                return { val: resReg, type: 'double' };
            }
            if (expr.name === 'retrocausal_ctc_dephasing') {
                const thArg = this.visitExpression(expr.arguments[0]);
                const thVal = thArg.type === 'i32' ? `double ${thArg.val}.0` : `double ${thArg.val}`;
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_retrocausal_ctc_dephasing(${thVal})`);
                return { val: resReg, type: 'double' };
            }

            // ─── 量子非欧几里德曲面体几何原语（投影/反转/度规/测地线）───
            if (expr.name === 'geodesic_distance') {
                const aArg = this.visitExpression(expr.arguments[0]);
                const bArg = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_geodesic_distance(%QObject* ${aArg.val}, %QObject* ${bArg.val})`);
                return { val: resReg, type: 'double' };
            }
            if (expr.name === 'inversion' || expr.name === 'hyperbolic_metric') {
                const xArg = this.visitExpression(expr.arguments[0]);
                const xVal = xArg.type === 'i32' ? `double ${xArg.val}.0` : `double ${xArg.val}`;
                const cname = expr.name === 'inversion' ? 'rvdl_inversion' : 'rvdl_hyperbolic_metric';
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @${cname}(${xVal})`);
                return { val: resReg, type: 'double' };
            }
            if (expr.name === 'hyperbolic_distance') {
                const xArg = this.visitExpression(expr.arguments[0]);
                const yArg = this.visitExpression(expr.arguments[1]);
                const xVal = xArg.type === 'i32' ? `double ${xArg.val}.0` : `double ${xArg.val}`;
                const yVal = yArg.type === 'i32' ? `double ${yArg.val}.0` : `double ${yArg.val}`;
                const resReg = this.nextReg();
                this.emit(`${resReg} = call double @rvdl_hyperbolic_distance(${xVal}, ${yVal})`);
                return { val: resReg, type: 'double' };
            }

            // ─── QChain 量子区块链内置函数 ──────────────────────────
            if (expr.name === 'qchain_wallet') {
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i8* @rvdl_qchain_wallet()`);
                return { val: resReg, type: 'i8*' };
            }

            if (expr.name === 'qchain_balance') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i64 @rvdl_qchain_balance(i8* ${arg.val})`);
                return { val: resReg, type: 'i64' };
            }

            if (expr.name === 'qchain_mint') {
                const addrArg = this.visitExpression(expr.arguments[0]);
                const amtArg = this.visitExpression(expr.arguments[1]);
                let amtVal = amtArg.val;
                if (amtArg.type === 'i32') {
                    const z = this.nextReg();
                    this.emit(`${z} = zext i32 ${amtArg.val} to i64`);
                    amtVal = z;
                }
                this.emit(`call void @rvdl_qchain_mint(i8* ${addrArg.val}, i64 ${amtVal})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.name === 'qchain_transfer') {
                const fromArg = this.visitExpression(expr.arguments[0]);
                const toArg = this.visitExpression(expr.arguments[1]);
                const amtArg = this.visitExpression(expr.arguments[2]);
                let amtVal = amtArg.val;
                if (amtArg.type === 'i32') {
                    const z = this.nextReg();
                    this.emit(`${z} = zext i32 ${amtArg.val} to i64`);
                    amtVal = z;
                }
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_qchain_transfer(i8* ${fromArg.val}, i8* ${toArg.val}, i64 ${amtVal})`);
                return { val: resReg, type: 'i32' };
            }

            if (expr.name === 'qchain_mine' || expr.name === 'qchain_height' || expr.name === 'qchain_verify') {
                const fn = expr.name === 'qchain_mine' ? 'rvdl_qchain_mine'
                         : expr.name === 'qchain_height' ? 'rvdl_qchain_height'
                         : 'rvdl_qchain_verify';
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @${fn}()`);
                return { val: resReg, type: 'i32' };
            }

            if (expr.name === 'qchain_qkd') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i8* @rvdl_qchain_qkd(i32 ${arg.val})`);
                return { val: resReg, type: 'i8*' };
            }

            if (expr.name === 'qchain_qdba') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_qchain_qdba(i32 ${arg.val})`);
                return { val: resReg, type: 'i32' };
            }

            if (expr.name === 'qchain_coin_mint') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call %QObject* @rvdl_qchain_coin_mint(i32 ${arg.val})`);
                this.trackTemporary(resReg, '%QObject*');
                return { val: resReg, type: '%QObject*' };
            }

            if (expr.name === 'qchain_coin_verify') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_qchain_coin_verify(%QObject* ${arg.val})`);
                return { val: resReg, type: 'i32' };
            }

            // ─── QChain 密码原语 / 抗超时空 / 时空加密 ──────────────
            if (expr.name === 'qchain_sha3' || expr.name === 'qchain_hash_unicode' ||
                expr.name === 'qchain_sign') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                const fn = expr.name === 'qchain_sha3' ? 'rvdl_qchain_sha3'
                         : expr.name === 'qchain_hash_unicode' ? 'rvdl_qchain_hash_unicode'
                         : 'rvdl_qchain_sign';
                this.emit(`${resReg} = call i8* @${fn}(i8* ${arg.val})`);
                return { val: resReg, type: 'i8*' };
            }

            if (expr.name === 'qchain_hmac' || expr.name === 'qchain_sign_verify' ||
                expr.name === 'qchain_mlkem_decaps') {
                const a = this.visitExpression(expr.arguments[0]);
                const b = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                const fn = expr.name === 'qchain_hmac' ? 'rvdl_qchain_hmac'
                         : expr.name === 'qchain_sign_verify' ? 'rvdl_qchain_sign_verify'
                         : 'rvdl_qchain_mlkem_decaps';
                if (expr.name === 'qchain_sign_verify') {
                    this.emit(`${resReg} = call i32 @${fn}(i8* ${a.val}, i8* ${b.val})`);
                    return { val: resReg, type: 'i32' };
                }
                this.emit(`${resReg} = call i8* @${fn}(i8* ${a.val}, i8* ${b.val})`);
                return { val: resReg, type: 'i8*' };
            }

            if (expr.name === 'qchain_sign_pubkey') {
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i8* @rvdl_qchain_sign_pubkey()`);
                return { val: resReg, type: 'i8*' };
            }

            if (expr.name === 'qchain_mlkem_encaps') {
                const arg = this.visitExpression(expr.arguments[0]);
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i8* @rvdl_qchain_mlkem_encaps(i8* ${arg.val})`);
                return { val: resReg, type: 'i8*' };
            }

            if (expr.name === 'qchain_causal_verify') {
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_qchain_causal_verify()`);
                return { val: resReg, type: 'i32' };
            }

            if (expr.name === 'qchain_cipher_encrypt' || expr.name === 'qchain_cipher_decrypt') {
                const seedArg = this.visitExpression(expr.arguments[0]);
                let seedVal = seedArg.val;
                if (seedArg.type === 'i32') {
                    const z = this.nextReg();
                    this.emit(`${z} = zext i32 ${seedArg.val} to i64`);
                    seedVal = z;
                }
                const dataArg = this.visitExpression(expr.arguments[1]);
                const resReg = this.nextReg();
                const fn = expr.name === 'qchain_cipher_encrypt' ? 'rvdl_qchain_cipher_encrypt' : 'rvdl_qchain_cipher_decrypt';
                this.emit(`${resReg} = call i8* @${fn}(i64 ${seedVal}, i8* ${dataArg.val})`);
                return { val: resReg, type: 'i8*' };
            }

            const scalarMathFns: Record<string, string[]> = {
                surrogate: ['double', 'double', 'double'],
                tanh_quantize: ['double', 'double', 'i32'],
                lif_step: ['double', 'double', 'double', 'double'],
                mellowmax2: ['double', 'double', 'double'],
                logsumexp2: ['double', 'double', 'double'],
                boltzmann2: ['double', 'double', 'double'],
                tnorm_luk: ['double', 'double'],
                tnorm_prod: ['double', 'double'],
                tnorm_godel: ['double', 'double'],
                polymer_weight: ['double', 'double', 'double'],
                polymer_mix_bound: ['double', 'double'],
                moe_groups: ['double'],
                moe_experts_per: ['double'],
                moe_virtual_experts: ['double'],
                moe_savings: ['double'],
                curvature_gate: ['double'],
                dual_fuse: ['double', 'double', 'double', 'double'],
                tau_alpha: ['double'],
                tau_generational: ['double', 'double'],
                tau_fold_depth: ['double', 'double'],
                tau_gear_ratio: ['double', 'double'],
                tau_route_cost: ['double', 'double', 'double', 'double'],
                tau_route_price: ['double', 'double', 'double'],
            };
            const sfTypes = scalarMathFns[expr.name];
            if (sfTypes) {
                const argVals = expr.arguments.map(a => this.visitExpression(a));
                const callArgs = argVals.map((a, i) => {
                    const want = sfTypes[i];
                    if (want === 'double' && a.type === 'i32') return `double ${a.val}.0`;
                    return `${want} ${a.val}`;
                }).join(', ');
                const res = this.nextReg();
                this.emit(`${res} = call double @rvdl_${expr.name}(${callArgs})`);
                return { val: res, type: 'double' };
            }

            // 外部 C 符号调用（extern 声明）
            const externSig = this.externSigs.get(expr.name);
            if (externSig) {
                const argVals = this.coerceArgs(
                    externSig.params,
                    expr.arguments.map(a => this.visitExpression(a))
                );
                const callArgs = externSig.params.map((pt, i) => `${pt} ${argVals[i].val}`).join(', ');
                if (externSig.ret === 'void') {
                    this.emit(`call void @${expr.name}(${callArgs})`);
                    return { val: 'void', type: 'void' };
                }
                const resReg = this.nextReg();
                this.emit(`${resReg} = call ${externSig.ret} @${expr.name}(${callArgs})`);
                // 传播 signedness：extern 返回 uint32 时，调用点上的 `%`、`>>`
                // 必须用 urem/lshr，否则会把无符号运算算成有符号（实测：
                // CSPRNG 的拒绝采样会产出负值）。
                const retSigned = externSig.retQ ? this.isSignedType(externSig.retQ) : true;
                return { val: resReg, type: externSig.ret, signed: retSigned };
            }

            if (expr.name.includes('::')) {
                const segs = expr.name.split('::');
                if (segs.length === 2 && this.importAliases.has(segs[0])) {
                    const alias = segs[0];
                    const funcName = segs[1];
                    const symbol = alias + '_' + funcName;
                    const sig = this.importSigs.get(expr.name);
                    const retType = sig ? sig.ret : 'i32';
                    const rawArgs = expr.arguments.map(a => this.visitExpression(a));
                    const paramTypes = sig
                        ? sig.params
                        : rawArgs.map(a => a.type);
                    const argVals = this.coerceArgs(paramTypes, rawArgs);
                    const callArgs = argVals.map((a, i) => `${paramTypes[i]} ${a.val}`).join(', ');
                    if (retType === 'void') {
                        this.emit(`call void @${symbol}(${callArgs})`);
                        return { val: 'void', type: 'void' };
                    }
                    const res = this.nextReg();
                    this.emit(`${res} = call ${retType} @${symbol}(${callArgs})`);
                    return { val: res, type: retType };
                }
            }

            const userFn = this.userFunctions.get(expr.name);
            if (userFn) {
                const retType = this.getLLVMType(userFn.returnType);
                const paramTypes = userFn.params.map(p => this.getLLVMType(p.type));
                const argVals = this.coerceArgs(
                    paramTypes,
                    expr.arguments.map(a => this.visitExpression(a))
                );
                // move 消费线性资源实参（值传递的 %QObject* / %Qubit*）：从符号表
                // 移除，避免调用方后续 emitCleanup 重复释放。
                expr.arguments.forEach((a, i) => {
                    if (paramTypes[i] === '%QObject*' || paramTypes[i] === '%Qubit*') {
                        this.consumeIfLinear(a);
                    }
                });
                const callArgs = argVals.map((a, i) => `${paramTypes[i]} ${a.val}`).join(', ');
                if (retType === 'void') {
                    this.emit(`call void @${expr.name}(${callArgs})`);
                    return { val: 'void', type: 'void' };
                }
                const res = this.nextReg();
                this.emit(`${res} = call ${retType} @${expr.name}(${callArgs})`);
                return { val: res, type: retType, signed: this.isSignedType(userFn.returnType) };
            }

            // 函数指针间接调用（fn<...> 变量：符号类型为 ret (params)*）
            const fnSym = this.getSymbol(expr.name);
            if (fnSym && fnSym.type.indexOf('(') > 0 && fnSym.type.endsWith(')*')) {
                const retType = fnSym.type.slice(0, fnSym.type.indexOf(' '));
                const fp = this.nextReg();
                this.emit(`${fp} = load ${fnSym.type}, ${fnSym.type}* ${fnSym.ptr}`);
                const argVals = expr.arguments.map(a => this.visitExpression(a));
                const callArgs = argVals.map(a => `${a.type} ${a.val}`).join(', ');
                if (retType === 'void') {
                    this.emit(`call void ${fp}(${callArgs})`);
                    return { val: 'void', type: 'void' };
                }
                const res = this.nextReg();
                this.emit(`${res} = call ${retType} ${fp}(${callArgs})`);
                return { val: res, type: retType };
            }

            // 函数变量间接调用（lambda / 闭包）
            if (fnSym && fnSym.type === 'i8*') {
                const argVals = expr.arguments.map(a => this.visitExpression(a));
                const argTypes = argVals.map(a => a.type);
                // 闭包对象指针（fnSym.ptr 存的是 i8* 闭包对象指针）
                const closurePtr = this.nextReg();
                this.emit(`${closurePtr} = load i8*, i8** ${fnSym.ptr}`);
                // fn_ptr 在闭包对象 offset 0（i8* 字段）
                const fnPtrPtr = this.nextReg();
                this.emit(`${fnPtrPtr} = bitcast i8* ${closurePtr} to i8**`);
                const fnRaw = this.nextReg();
                this.emit(`${fnRaw} = load i8*, i8** ${fnPtrPtr}`);
                const fnTyped = this.nextReg();
                this.emit(`${fnTyped} = bitcast i8* ${fnRaw} to i32 (i8*, ${argTypes.join(', ')})*`);
                const res = this.nextReg();
                this.emit(`${res} = call i32 ${fnTyped}(i8* ${closurePtr}, ${argVals.map(a => a.type + ' ' + a.val).join(', ')})`);
                return { val: res, type: 'i32' };
            }

            throw new Error(`IR Error: Unknown function '${expr.name}'`);
        }

        if (expr.type === 'IndexExpression') {
            // 定长数组索引：a[i] → GEP [N x T] + load（数组符号 / 全局数组直接取指针）
            if (expr.object.type === 'Identifier') {
                let arrType: string | null = null;
                let arrPtr: string | null = null;
                const sym = this.getSymbol(expr.object.name);
                if (sym && sym.type.startsWith('[')) {
                    arrType = sym.type;
                    arrPtr = sym.ptr;
                } else if (this.globalVars.has(expr.object.name)) {
                    const decl = this.globalVars.get(expr.object.name)!;
                    const lt = this.getLLVMType(decl.varType);
                    if (lt.startsWith('[')) {
                        arrType = lt;
                        arrPtr = '@' + expr.object.name;
                    }
                }
                if (arrType && arrPtr) {
                    const m = arrType.match(/^\[(\d+) x (.+)\]$/);
                    const elemType = m ? m[2] : 'i32';
                    const idx = this.visitExpression(expr.indices[0]);
                    const gep = this.nextReg();
                    this.emit(`${gep} = getelementptr ${arrType}, ${arrType}* ${arrPtr}, i32 0, i32 ${idx.val}`);
                    const loadReg = this.nextReg();
                    this.emit(`${loadReg} = load ${elemType}, ${elemType}* ${gep}`);
                    return { val: loadReg, type: elemType };
                }
            }
            const obj = this.visitExpression(expr.object);
            const idx0 = expr.indices.length > 0 ? this.visitExpression(expr.indices[0]).val : '0';
            const idx1 = expr.indices.length > 1 ? this.visitExpression(expr.indices[1]).val : '0';
            if (obj.type === 'i8*') {
                // 字符串遍历：s[i] → getelementptr + load i8（返回字节字符）
                const gepReg = this.nextReg();
                this.emit(`${gepReg} = getelementptr i8, i8* ${obj.val}, i32 ${idx0}`);
                const loadReg = this.nextReg();
                this.emit(`${loadReg} = load i8, i8* ${gepReg}`);
                return { val: loadReg, type: 'i8' };
            }
            const resReg = this.nextReg();
            this.emit(`${resReg} = call i32 @rvdl_lattice_ref(${obj.type} ${obj.val}, i32 ${idx0}, i32 ${idx1})`);
            return { val: resReg, type: 'i32' };
        }

        if (expr.type === 'MemberExpression') {
            const obj = this.visitExpression(expr.object);
            const bareType = obj.type.replace(/^%/, '').replace(/\*$/, '');
            const formName = this.formTypeToName.get(bareType);
            if (formName) {
                const fields = this.getFormFields(formName);
                const field = fields.find(f => f.name === expr.property);
                if (field && !expr.isMethodCall) {
                    const gep = this.nextReg();
                    this.emit(`${gep} = getelementptr %${bareType}, ${obj.type} ${obj.val}, i32 0, i32 ${field.index}`);
                    const loadReg = this.nextReg();
                    this.emit(`${loadReg} = load ${field.llvmType}, ${field.llvmType}* ${gep}`);
                    // 位域：lshr 到低位 + and 掩码（无符号位域）
                    if (field.bitWidth !== undefined) {
                        const shifted = this.nextReg();
                        this.emit(`${shifted} = lshr ${field.llvmType} ${loadReg}, ${field.bitOffset ?? 0}`);
                        const mask = ((1n << BigInt(field.bitWidth)) - 1n).toString();
                        const masked = this.nextReg();
                        this.emit(`${masked} = and ${field.llvmType} ${shifted}, ${mask}`);
                        return { val: masked, type: field.llvmType };
                    }
                    return { val: loadReg, type: field.llvmType };
                }

                const methodName = expr.property;
                const inherent = this.implMethods.get(formName) || [];
                const inherentMethod = inherent.find(m => m.name === methodName);
                if (inherentMethod && inherentMethod.body) {
                    const retType = this.getLLVMType(inherentMethod.returnType);
                    const argVals = expr.arguments.map(a => this.visitExpression(a));
                    const argTypes = [obj.type, ...inherentMethod.params.map(p => this.getLLVMType(p.type))];
                    const callArgs = [obj.type + ' ' + obj.val, ...argVals.map((a, i) => argTypes[i + 1] + ' ' + a.val)].join(', ');
                    if (retType === 'void') {
                        this.emit(`call void @${this.mangleMethod(formName, methodName)}(${callArgs})`);
                        return { val: 'void', type: 'void' };
                    }
                    const res = this.nextReg();
                    this.emit(`${res} = call ${retType} @${this.mangleMethod(formName, methodName)}(${callArgs})`);
                    return { val: res, type: retType };
                }

                for (const [traitName, impls] of this.traitImpls) {
                    if (!impls.has(formName)) continue;
                    const trait = this.traits.get(traitName);
                    if (!trait) continue;
                    const traitMethods: FnDecl[] = [];
                    for (const rank of trait.ranks) traitMethods.push(...rank.methods);
                    const slot = traitMethods.findIndex(m => m.name === methodName);
                    if (slot < 0) continue;
                    const tm = traitMethods[slot];
                    const retType = this.getLLVMType(tm.returnType);
                    const vtType = '%' + this.mangleForm(traitName) + '.vtable';

                    const vtPtrPtr = this.nextReg();
                    this.emit(`${vtPtrPtr} = getelementptr %${bareType}, ${obj.type} ${obj.val}, i32 0, i32 0`);
                    const vtRaw = this.nextReg();
                    this.emit(`${vtRaw} = load i8*, i8** ${vtPtrPtr}`);
                    const vtTyped = this.nextReg();
                    this.emit(`${vtTyped} = bitcast i8* ${vtRaw} to ${vtType}*`);
                    const slotPtr = this.nextReg();
                    this.emit(`${slotPtr} = getelementptr ${vtType}, ${vtType}* ${vtTyped}, i32 0, i32 ${slot}`);
                    const fnRaw = this.nextReg();
                    this.emit(`${fnRaw} = load i8*, i8** ${slotPtr}`);

                    const selfType = '%' + this.mangleForm(formName) + '*';
                    const paramTypes = tm.params.map(p => this.getLLVMType(p.type));
                    const fnType = `${retType} (${[selfType, ...paramTypes].join(', ')})*`;
                    const fnTyped = this.nextReg();
                    this.emit(`${fnTyped} = bitcast i8* ${fnRaw} to ${fnType}`);

                    const argVals = expr.arguments.map(a => this.visitExpression(a));
                    const callArgs = [selfType + ' ' + obj.val, ...argVals.map((a, i) => paramTypes[i] + ' ' + a.val)].join(', ');
                    if (retType === 'void') {
                        this.emit(`call void ${fnTyped}(${callArgs})`);
                        return { val: 'void', type: 'void' };
                    }
                    const res = this.nextReg();
                    this.emit(`${res} = call ${retType} ${fnTyped}(${callArgs})`);
                    return { val: res, type: retType };
                }
            }

            if (expr.property === 'export') {
                const pathArg = this.visitExpression(expr.arguments[0]);
                this.emit(`call void @rvdl_qkm_export(${obj.type} ${obj.val}, i8* ${pathArg.val})`);
                return { val: 'void', type: 'void' };
            }

            if (expr.property === 'measure') {
                const resReg = this.nextReg();
                this.emit(`${resReg} = call i32 @rvdl_measure_object(%QObject* ${obj.val})`);
                return { val: resReg, type: 'i32' };
            }
        }

        throw new Error(`IR Error: Unknown expression type '${expr.type}'`);
    }

    private visitFunctionDeclaration(func: FunctionDeclaration) {
        const llvmRetType = this.getLLVMType(func.returnType);
        const paramTypes = func.params.map(p => this.getLLVMType(p.type));

        this.output.push(``);
        const paramsStr = paramTypes.map((t, i) => `${t} %arg${i}`).join(', ');

        // 函数属性：系统级（section/naked）+ 经典编译属性（inline/noinline/pure/cold/noreturn/export）。
        // 量子门属性（gate/undo/steer/unitary/measure）已在 parser 层分离到 func.quantum，此处不受影响。
        // 历史：place/raw 曾作为 section/naked 的别名，二者兼容。
        let fnAttrs = '';
        let fnPrefix = '';
        if (func.attributes && func.attributes.length > 0) {
            const parts: string[] = [];
            for (const a of func.attributes) {
                switch (a.name) {
                    case 'section': case 'place':
                        if (a.value) parts.push(`section "${a.value}"`);
                        break;
                    case 'naked': case 'raw':
                        parts.push('naked');
                        break;
                    case 'inline':       parts.push('alwaysinline'); break;
                    case 'noinline':     parts.push('noinline');     break;
                    case 'pure':         parts.push('readnone');     break;
                    case 'readonly':     parts.push('readonly');     break;
                    case 'cold':         parts.push('cold');         break;
                    case 'hot':          parts.push('hot');          break;
                    case 'noreturn':     parts.push('noreturn');     break;
                    case 'export':       fnPrefix = 'dllexport ';    break;
                    default: break;
                }
            }
            if (parts.length > 0) fnAttrs = ' ' + parts.join(' ');
        }
        // 裸函数（@[naked]）：函数体只允许 asm{} 块，无栈帧、无 prologue/epilogue。
        // LLVM naked 函数体只能是一条 asm + unreachable，此处直接发射完整汇编体。
        const isNaked = (func.attributes ?? []).some(a => a.name === 'naked' || a.name === 'raw');
        if (isNaked && func.body.length > 0 && func.body.every(s => s.type === 'AsmStatement')) {
            this.output.push(`define ${fnPrefix}${llvmRetType} @${func.name}(${paramsStr})${fnAttrs} {`);
            this.output.push(`entry:`);
            for (const s of func.body) {
                const tpl = (s as any).template.replace(/"/g, '\\"').replace(/\n/g, '\\0A');
                this.output.push(`  call void asm sideeffect "${tpl}", ""()`);
            }
            this.output.push(`  unreachable`);
            this.output.push(`}`);
            this.currentNoise = null;
            return;
        }

        this.output.push(`define ${fnPrefix}${llvmRetType} @${func.name}(${paramsStr})${fnAttrs} {`);
        this.output.push(`entry:`);
        this.scopes = [{ symbols: new Map(), temporaries: [] }];
        // consumedSymbols 是「当前函数内已 move 消费」的线性资源名集合，必须按函数
        // 重置。否则前一个函数 move 消费过同名符号（如 `return state` 标记了 "state"），
        // 会污染后续函数的 emitCleanup —— 后续函数同名的参数/局部变量会被错误跳过释放，
        // 造成 %QObject* 泄漏（quality_of 的 state 参数即因此未 release）。
        this.consumedSymbols.clear();
        this.regCount = 1;
        this.allocas = [];
        this.isBlockTerminated = false;
        // 从 @[noise]/@[coherence] 提取噪声通道（门操作后注入）
        this.currentNoise = this.computeNoise(func.physical);

        const entryIndex = this.output.length;

        func.params.forEach((p, i) => {
            const ptr = '%' + p.name + '_ptr';
            this.allocas.push(` ${ptr} = alloca ${paramTypes[i]}`);
            this.emit(`store ${paramTypes[i]} %arg${i}, ${paramTypes[i]}* ${ptr}`);
            this.setSymbol(p.name, { ptr: ptr, type: paramTypes[i], signed: this.isSignedType(p.type) });
        });

        for (const stmt of func.body) {
            this.visitStatement(stmt);
        }

        this.exitScope();

        if (!this.isBlockTerminated) {
            if (llvmRetType === 'void') this.emit(`ret void`);
            else this.emit(`ret ${llvmRetType} ${this.defaultRetValue(llvmRetType)}`);
            this.isBlockTerminated = true;
        }

        this.output.splice(entryIndex, 0, ...this.allocas);
        this.output.push(`}`);

        // 函数体结束，重置噪声通道
        this.currentNoise = null;
    }

    /**
     * 从 @[noise]/@[coherence] 物理特性推导噪声通道列表。
     *
     * 返回**多个**独立信道，由 emitNoiseIfNeeded 依次注入（原先只能返回单通道，
     * 被迫把不同物理来源折叠成一个强度）。
     */
    private computeNoise(physical?: any): { channel: number; param: number }[] | null {
        if (!physical) return null;
        if (physical.noise) {
            const chMap: Record<string, number> = {
                depolarizing: 0, phase_damping: 1, amplitude_damping: 2, bit_flip: 3,
            };
            const ch = chMap[physical.noise];
            if (ch === undefined) return null;
            // 强度未由 @[noise(...)] 指定时：若有 @[coherence] 则从 T1/T2 物理推导，
            // 否则回退到保守默认 1%。
            const param = physical.coherence
                ? defaultNoiseStrength(physical.coherence, ch)
                : DEFAULT_NOISE_PARAM;
            return [{ channel: ch, param }];
        }
        if (physical.coherence) {
            // 相干时间 → 噪声通道（多通道）。
            //
            // 真实器件同时存在两个**独立**的退相干来源：
            //   T1（能量弛豫 / 振幅衰减） → 振幅阻尼（channel 2）
            //   T_φ（纯退相 / 相位信息丢失）→ 相位阻尼（channel 1）
            // 二者由 T2 关联：   1/T2 = 1/(2·T1) + 1/T_φ
            // 故纯退相速率：     1/T_φ = 1/T2 − 1/(2·T1)
            //
            // 旧实现只能返回一个通道：先是完全丢弃 T2（只用 1/T1），后虽改为用 T2
            // 标定强度，却仍把两个物理来源混成一个数。现在返回两个独立信道，
            // 各自用自己的时间常数，物理上更贴合真实器件。
            //
            // 语义层保证 0 < T2 ≤ T1（E-PHY），故 1/T_φ ≥ 1/T2 − 1/(2·T2) > 0，
            // 即纯退相通道通常存在；T2 缺如（<=0）时只输出弛豫通道。
            const t1 = Math.max(physical.coherence.t1, 1.0);
            const t2 = physical.coherence.t2;
            const channels: { channel: number; param: number }[] = [];

            // (1) T1 弛豫 → 振幅阻尼
            channels.push({ channel: 2, param: Math.min(1.0, 1.0 / t1) });

            // (2) T2 中的纯退相部分 → 相位阻尼
            if (t2 > 0) {
                const invTphi = 1.0 / Math.max(t2, 1.0) - 1.0 / (2.0 * t1);
                if (invTphi > 0) {
                    channels.push({ channel: 1, param: Math.min(1.0, invTphi) });
                }
            }
            return channels;
        }
        return null;
    }

    /** 若当前函数带噪声元数据，则在门后依次注入各噪声通道调用（可多通道） */
    private emitNoiseIfNeeded(qubitType: string, qubitVal: string): void {
        if (!this.currentNoise || this.currentNoise.length === 0) return;
        for (const n of this.currentNoise) {
            this.emit(`call void @__quantum__qis__apply_noise(${qubitType} ${qubitVal}, i32 ${n.channel}, double ${n.param.toFixed(6)})`);
        }
    }

    private getLLVMType(ravendrelType: string): string {
        switch (ravendrelType) {
            case 'int8': case 'uint8': case 'char': return 'i8';
            case 'int16': case 'uint16': return 'i16';
            case 'int': case 'int32': case 'uint32': return 'i32';
            case 'int64': case 'uint64': return 'i64';
            case 'float': return 'float';
            case 'double': return 'double';
            case 'complex64': return '{ float, float }';
            case 'complex128': return '{ double, double }';
            case 'string': return 'i8*';
            case 'Qubit': return '%Qubit*';
            case 'QObject': return '%QObject*';
            case 'QModel': return '%QModel*';
            case 'QReservoir': return '%QReservoir*';
            case 'void': return 'void';
            default:
                if (ravendrelType.startsWith('cap<')) {
                    const inner = ravendrelType.slice(4, -1);
                    return this.getLLVMType(inner) + '*'; // 能力（typed pointer）
                }
                if (this.forms.has(ravendrelType)) return '%' + this.mangleForm(ravendrelType) + '*';
                if (ravendrelType.startsWith('lattice<')) return '%Lattice*';
                if (ravendrelType.startsWith('arr<')) {
                    // arr<T, N> → [N x T]（定长数组）
                    const inner = ravendrelType.slice(4, -1);
                    const parts = inner.split(',');
                    const elem = this.getLLVMType(parts[0].trim());
                    const len = parts.length > 1 ? parts[1].trim() : '0';
                    return `[${len} x ${elem}]`;
                }
                if (ravendrelType.startsWith('(') && ravendrelType.includes(')->')) return this.getFnPtrType(ravendrelType);
                if (ravendrelType.startsWith('fn<')) return this.getFnPtrType(ravendrelType);
                throw new Error(`IR Error: Unknown type '${ravendrelType}'`);
        }
    }

    /** 函数指针类型：(p1, p2)->ret 或 fn<ret(p1, p2)> → ret (p1, p2)* */
    private getFnPtrType(ravendrelType: string): string {
        let paramsStr: string;
        let retStr: string;
        if (ravendrelType.startsWith('fn<')) {
            const inner = ravendrelType.slice(3, -1);
            const openParen = inner.indexOf('(');
            retStr = inner.slice(0, openParen < 0 ? inner.length : openParen).trim();
            paramsStr = openParen >= 0 ? inner.slice(openParen + 1, inner.lastIndexOf(')')).trim() : '';
        } else {
            const arrowIdx = ravendrelType.indexOf(')->');
            paramsStr = ravendrelType.slice(1, arrowIdx).trim();
            retStr = ravendrelType.slice(arrowIdx + 3).trim();
        }
        const params = paramsStr ? paramsStr.split(',').map(s => this.getLLVMType(s.trim())) : [];
        const ret = this.getLLVMType(retStr);
        return `${ret} (${params.join(', ')})*`;
    }

    /**
     * 发射 EDX:EAX 双 32 位输出内联汇编，组合为单个 i64 返回（rdmsr / rdtsc / xgetbv）。
     * x86-64 下 `=A` 约束已不再表示 edx:eax 对，必须用两个独立输出 `={eax},={edx}` 再组合。
     */
    private emitEdxEaxAsm(template: string, constraints: string, inputArgs: { type: string; val: string }[]): string {
        const res = this.nextReg();
        const inStr = inputArgs.map(a => `${a.type} ${a.val}`).join(', ');
        this.emit(`${res} = call { i32, i32 } asm sideeffect "${template}", "${constraints}"(${inStr})`);
        const lo = this.nextReg(), hi = this.nextReg();
        this.emit(`${lo} = extractvalue { i32, i32 } ${res}, 0`);
        this.emit(`${hi} = extractvalue { i32, i32 } ${res}, 1`);
        const zlo = this.nextReg(), zhi = this.nextReg();
        this.emit(`${zlo} = zext i32 ${lo} to i64`);
        this.emit(`${zhi} = zext i32 ${hi} to i64`);
        const shifted = this.nextReg();
        this.emit(`${shifted} = shl i64 ${zhi}, 32`);
        const combined = this.nextReg();
        this.emit(`${combined} = or i64 ${zlo}, ${shifted}`);
        return combined;
    }
}