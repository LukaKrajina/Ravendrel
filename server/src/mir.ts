import type {
    Program,
    Statement,
    Expression,
} from './ast';


export type LocalId = number;
export type BlockId = number;





export interface MirPlace {
    local: LocalId;
    projection: MirProjection[];
}

export type MirProjection =
    | { kind: 'Field'; name: string }
    | { kind: 'Deref' }
    | { kind: 'Index'; index: LocalId };

export type MirOperand =
    | { kind: 'Copy'; place: MirPlace }
    | { kind: 'Move'; place: MirPlace }
    | { kind: 'Ref'; place: MirPlace; mut: boolean }
    | { kind: 'Const'; value: MirConst };

export type MirConst =
    | { kind: 'Int'; value: number; ty: string }
    | { kind: 'Float'; value: number; ty: string }
    | { kind: 'Str'; value: string }
    | { kind: 'Char'; value: string }
    | { kind: 'Bool'; value: boolean };


export type MirRvalue =
    | { kind: 'Use'; operand: MirOperand }
    | { kind: 'Ref'; place: MirPlace; mut: boolean; ty: string }
    | { kind: 'Binary'; op: string; lhs: MirOperand; rhs: MirOperand; ty: string }
    | { kind: 'Unary'; op: string; arg: MirOperand; ty: string }
    | { kind: 'Call'; target: string; args: MirOperand[]; retTy: string | null }
    | { kind: 'NewObject'; className: string; args: MirOperand[]; ty: string }
    | { kind: 'Member'; object: MirOperand; property: string; ty: string; fieldIndex?: number; fieldType?: string }
    | { kind: 'Asm'; template: string }
    | { kind: 'Array'; elements: MirOperand[]; elemTy: string }
    | { kind: 'ArrayIndex'; object: MirOperand; index: MirOperand; elemTy: string; arrTy: string }
    | { kind: 'QAlloc'; ty: string }
    | { kind: 'QGate'; gate: string; args: MirOperand[] }
    | { kind: 'QMeasure'; arg: MirOperand; ty: string };


export type MirStatement =
    | { kind: 'Assign'; place: MirPlace; rvalue: MirRvalue }
    | { kind: 'Drop'; place: MirPlace }
    | { kind: 'Consume'; place: MirPlace };

export type MirTerminator =
    | { kind: 'Goto'; target: BlockId }
    | { kind: 'SwitchInt'; discriminant: MirOperand; targets: Array<{ value: number | null; target: BlockId }> }
    | { kind: 'Return'; value: MirOperand | null }
    | { kind: 'Call'; target: string; args: MirOperand[]; destination: MirPlace | null; next: BlockId }
    | { kind: 'Unreachable' };


export interface MirBasicBlock {
    id: BlockId;
    statements: MirStatement[];
    terminator: MirTerminator | null;
}

export interface MirLocalDecl {
    id: LocalId;
    name: string | null;
    ty: string;
    mut: boolean;



    linear: boolean;
    temporary: boolean;
    line: number;
    column: number;
}

export interface MirBody {
    owner: string;
    locals: MirLocalDecl[];
    blocks: MirBasicBlock[];
    params: LocalId[];
    returnTy: string | null;
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
    forms?: { name: string; fields: { name: string; ty: string }[]; packed?: boolean }[];
}


import {
    FunctionDeclaration,
    Param,
} from './ast';




function isTopLevelItem(node: any): boolean {
    if (node.type === 'VariableDeclarationList') {
        return node.varType !== 'auto' && node.varType !== 'let';
    }
    return ['ModuleDecl', 'UseDecl', 'FormDecl', 'FlavorDecl', 'ImplDecl', 'TraitDecl',
            'TemplateDecl', 'ImportDecl', 'RequiresDecl', 'ExternDecl', 'GlobalVarDecl'].includes(node.type);
}





const LINEAR_TYPES = new Set<string>([
    'Qubit', 'QObject',
    'QuantumRegister', 'DiracState', 'BellState',
]);

const GATE_FNS = new Set<string>([
    'x', 'h', 'y', 'z', 'rz', 'rx', 'ry', 's', 't',
    'cnot', 'toffoli', 'swap', 'qft', 'iqft', 'braid',
    'cx', 'ch', 'crz', 'cswap', 'c_toffoli', 'cqft', 'cbraid',
]);

const MEASURE_FNS = new Set<string>(['measure', 'measure_x', 'measure_y']);







const QOBJ_BORROW_FNS = new Set<string>([
    'qgate_h', 'qgate_x', 'qgate_rz', 'qgate_cnot',
    'qexpect_z', 'qattention',
    'qobj_num_qubits',
    'qgate_cnot_pair',
    'geodesic_distance',
    'qlm_forward',
]);







const QOBJ_MEASURE_FNS = new Set<string>(['qmeasure']);




const BUILTIN_RET_TY: Record<string, string> = {
    'basis_state': 'QObject',
    'encode_text': 'QObject', 'encode_image': 'QObject', 'encode_pixel': 'QObject',
    'encode_amplitudes': 'QObject',
    'encode_adaptive': 'QObject',
    'qobj_num_qubits': 'int32',
    'qgate_cnot_pair': 'void',
    'rvdl_encode_string': 'QObject', 'rvdl_decode_string': 'string',
    'qlm_invoke': 'QModel', 'qlm_load': 'QModel', 'qlm_forward': 'void',
    'mind_read': 'QObject', 'mind_train': 'void', 'mind_feedback': 'void',
    'veda_qlm_train': 'void',
    'surrogate': 'double', 'tanh_quantize': 'double', 'lif_step': 'double',
    'mellowmax2': 'double', 'logsumexp2': 'double', 'boltzmann2': 'double',
    'tnorm_luk': 'double', 'tnorm_prod': 'double', 'tnorm_godel': 'double',
    'polymer_weight': 'double', 'polymer_mix_bound': 'double',
    'tau_alpha': 'double', 'tau_generational': 'double', 'tau_fold_depth': 'double',
    'tau_gear_ratio': 'double', 'tau_route_cost': 'double', 'tau_route_price': 'double',
    'rvdl_sys_call': 'int32', 'rvdl_sys_calld': 'double',
    'rvdl_sys_log': 'void', 'rvdl_sys_logi': 'int32', 'rvdl_sys_callp': 'cap<uint8>',
    'rvdl_gc_alloc': 'cap<int32>', 'rvdl_gc_free': 'void',
    'rvdl_qms_gap': 'double', 'rvdl_mix_bound': 'double', 'rvdl_qms_conc': 'double',
    'sync_load': 'int32', 'sync_add': 'int32', 'sync_cas': 'int32',
    'sync_store': 'void', 'sync_lock': 'int32', 'sync_unlock': 'void',
    'outb': 'void', 'inb': 'int32', 'outw': 'void', 'inw': 'int32',
    'outl': 'void', 'inl': 'int32', 'addr': 'int32',
    'read_cr0': 'uint64', 'read_cr2': 'uint64', 'read_cr3': 'uint64', 'read_cr4': 'uint64',
    'write_cr0': 'void', 'write_cr3': 'void', 'invlpg': 'void',
    'rdmsr': 'uint64', 'wrmsr': 'void', 'cpuid': 'void',
    'rdtsc': 'uint64', 'read_rflags': 'uint64', 'xgetbv': 'uint64',
    'volatile_load': 'int32', 'volatile_store': 'void',
    'complex': 'complex128', 'real': 'double', 'imag': 'double',
    'cabs': 'double', 'conj': 'complex128',
    'qchain_wallet': 'string', 'qchain_mint': 'void', 'qchain_transfer': 'int32',
    'qchain_balance': 'uint64', 'qchain_mine': 'int32', 'qchain_height': 'int32',
    'qchain_verify': 'int32', 'qchain_qkd': 'string', 'qchain_qdba': 'int32',
    'qchain_coin_mint': 'QObject', 'qchain_coin_verify': 'int32',
    'qchain_sha3': 'string', 'qchain_hmac': 'string', 'qchain_hash_unicode': 'string',
    'qchain_sign': 'string', 'qchain_sign_verify': 'int32', 'qchain_sign_pubkey': 'string',
    'qchain_mlkem_encaps': 'string', 'qchain_mlkem_decaps': 'string',
    'qchain_causal_verify': 'int32',
    'qchain_cipher_encrypt': 'string', 'qchain_cipher_decrypt': 'string',
    'qrc_new': 'QReservoir', 'qrc_train': 'void', 'qrc_train_ex': 'void', 'qrc_release': 'void',
    'qrc_probe': 'QObject', 'qrc_predict': 'QObject',
    'dla_dim': 'int32',
    'qstate_entropy': 'double', 'qstate_fidelity': 'double',
    'qstate_purity': 'double', 'qstate_linear_entropy': 'double',
    'qstate_renyi_entropy': 'double',
    'math_dot': 'double', 'math_norm': 'double',
    'qattention': 'double',
    'shannon4': 'double', 'shannon8': 'double',
    'qgate_h': 'void', 'qgate_x': 'void', 'qgate_rz': 'void', 'qgate_cnot': 'void',
    'qexpect_z': 'double',
    'qmeasure': 'int32',
    'retrocausal_imax': 'double', 'retrocausal_idoe': 'double',
    'retrocausal_q_capacity': 'double', 'retrocausal_c_capacity': 'double',
    'retrocausal_q_one_shot': 'double',
    'retrocausal_gain': 'double', 'retrocausal_deformed': 'double',
    'retrocausal_ctc_q_capacity': 'double', 'retrocausal_ctc_c_capacity': 'double',
    'retrocausal_ctc_gain': 'double', 'retrocausal_ctc_dephasing': 'double',
    'geodesic_distance': 'double', 'inversion': 'double',
    'hyperbolic_metric': 'double', 'hyperbolic_distance': 'double',
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
    'lattice_rank': 'int32', 'lattice_size': 'int32', 'lattice_boundary': 'int32',
    'rvdl_lattice_ref': 'int32',
};

const METHOD_RET_TY: Record<string, string> = {
    'measure': 'int32',
    'export': 'void',
};

function constInt(expr: Expression): number {
    if (expr.type === 'NumberLiteral') return expr.value;
    if (expr.type === 'CharLiteral') return expr.value.charCodeAt(0);
    return 0;
}

export class MirBuilder {
    private locals: MirLocalDecl[] = [];
    private blocks: MirBasicBlock[] = [];
    private currentBlock: BlockId = 0;
    private varEnv = new Map<string, LocalId>();
    private loopStack: Array<{ breakTo: BlockId; continueTo: BlockId }> = [];
    private owner: string;
    private userFuncRet: Map<string, string> = new Map();
    private userGateFns: Set<string> = new Set();
    private userMeasureFns: Set<string> = new Set();
    private formFields: Map<string, { name: string; ty: string }[]> = new Map();
    private packedForms: Set<string> = new Set();
    private flavorMembers: Map<string, number> = new Map();
    private globalSymbols: Set<string> = new Set();

    constructor(owner: string = 'rvdl_main') {
        this.owner = owner;
        this.blocks.push({ id: 0, statements: [], terminator: null });
    }


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
                return { kind: 'Const', value: { kind: 'Int', value: 0, ty: 'i8*' } };

            case 'Dereference':
                return this.lowerExpression(expr.target);

            case 'AddressOf': {
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
                for (const op of expr.operands) this.lowerExpression(op);
                const p = this.materialize(
                    { kind: 'Call', target: 'native', args: [], retTy: 'void' },
                    'void', expr.line, expr.column);
                return { kind: 'Copy', place: p };
            }

            case 'ArrayLiteral': {
                const elements = expr.elements.map(e => this.lowerExpression(e));
                const elemTy = elements.length > 0 ? this.operandType(elements[0]) : 'int32';
                const p = this.materialize(
                    { kind: 'Array', elements, elemTy },
                    `arr<${elemTy}, ${elements.length}>`, expr.line, expr.column);
                return { kind: 'Copy', place: p };
            }

            case 'Identifier': {
                if (this.flavorMembers.has(expr.name)) {
                    const tmp = this.materialize(
                        { kind: 'Use', operand: { kind: 'Const', value: { kind: 'Int', value: this.flavorMembers.get(expr.name)!, ty: 'int32' } } },
                        'int32', expr.line, expr.column);
                    return { kind: 'Copy', place: tmp };
                }
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






            case 'LogicalExpression': {
                const result = this.newLocal(null, 'bool', true, true, expr.line, expr.column);
                const lhs = this.lowerExpression(expr.left);

                const shortCircuitBlock = this.newBlock();
                const resultBlock = this.newBlock();
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

                this.enterBlock(resultBlock);
                this.push({
                    kind: 'Assign',
                    place: this.place(result),
                    rvalue: { kind: 'Use', operand: { kind: 'Const', value: { kind: 'Bool', value: !isAnd } } },
                });
                this.terminate({ kind: 'Goto', target: afterBlock });

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
                    const args = (expr.arguments ?? []).map(a => this.lowerExpression(a));
                    const retTy = METHOD_RET_TY[expr.property] ?? 'int32';
                    if (expr.property === 'measure' && object.kind === 'Copy' &&
                        this.locals[object.place.local].linear) {
                        this.push({ kind: 'Consume', place: object.place });
                    }
                    const p = this.materialize(
                        { kind: 'Call', target: expr.property, args, retTy }, retTy, expr.line, expr.column);
                    return { kind: 'Copy', place: p };
                }
                let fieldIndex: number | undefined;
                let fieldType: string | undefined;
                if (expr.object.type === 'Identifier') {
                    const id = this.varEnv.get(expr.object.name);
                    if (id !== undefined && id < this.locals.length) {
                        const fields = this.formFields.get(this.locals[id].ty);
                        if (fields) {
                            const idx = fields.findIndex(f => f.name === expr.property);
                            if (idx >= 0) {
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
                if (expr.object.type === 'Identifier') {
                    const id = this.varEnv.get(expr.object.name);
                    if (id !== undefined && id < this.locals.length) {
                        const ty = this.locals[id].ty;
                        if (ty === 'string') {
                            return this.lowerCall('rvdl_str_index', [expr.object, ...expr.indices], expr.line, expr.column);
                        }
                        if (ty.startsWith('arr<')) {
                            const elemTy = ty.slice(4, -1).split(',')[0].trim();
                            const obj: MirOperand = { kind: 'Ref', place: this.place(id), mut: false };
                            const index = this.lowerExpression(expr.indices[0]);
                            const p = this.materialize(
                                { kind: 'ArrayIndex', object: obj, index, elemTy, arrTy: ty },
                                elemTy, expr.line, expr.column);
                            return { kind: 'Copy', place: p };
                        }
                    }
                }
                return this.lowerCall('rvdl_lattice_ref', [expr.object, ...expr.indices], expr.line, expr.column);
            }

            case 'FunctionExpression': {
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

    private lowerBorrowArg(expr: Expression): MirOperand {
        if (expr.type === 'Identifier') {
            const id = this.varEnv.get(expr.name);
            if (id === undefined) {
                throw new Error(`MIR Error: unresolved variable '${expr.name}' (line ${expr.line})`);
            }
            return { kind: 'Ref', place: this.place(id), mut: true };
        }
        return this.lowerExpression(expr);
    }

    private retTyOf(name: string): string {
        const builtin = BUILTIN_RET_TY[name];
        if (builtin !== undefined) return builtin;
        const user = this.userFuncRet.get(name);
        if (user !== undefined) return user;
        return 'int32';
    }

    private lowerCall(name: string, args: Expression[], line: number, column: number): MirOperand {
        if (name === 'alloc') {
            const p = this.materialize({ kind: 'QAlloc', ty: 'Qubit' }, 'Qubit', line, column);
            return { kind: 'Copy', place: p };
        }

        if (GATE_FNS.has(name) || this.userGateFns.has(name)) {
            const lowered = args.map(a => this.lowerBorrowArg(a));
            const p = this.materialize({ kind: 'QGate', gate: name, args: lowered }, 'void', line, column);
            return { kind: 'Copy', place: p };
        }

        if (QOBJ_BORROW_FNS.has(name)) {
            const lowered = args.map(a => this.lowerBorrowArg(a));
            const retTy = this.retTyOf(name);
            const p = this.materialize(
                { kind: 'Call', target: name, args: lowered, retTy }, retTy, line, column);
            return { kind: 'Copy', place: p };
        }

        if (QOBJ_MEASURE_FNS.has(name)) {
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


    private lowerStatement(stmt: Statement): void {
        switch (stmt.type) {
            case 'VariableDeclarationList': {
                for (const d of stmt.declarations) this.lowerStatement(d);
                break;
            }
            case 'VariableDeclaration': {
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
                if (stmt.target && stmt.target.type === 'Dereference') {
                    this.lowerExpression(stmt.target.target);
                    this.lowerExpression(stmt.value);
                    break;
                }
                if (stmt.target && (stmt.target.type === 'IndexExpression' || stmt.target.type === 'MemberExpression')) {
                    const targetOperand = this.lowerExpression(stmt.target);
                    const val = this.lowerExpression(stmt.value);
                    if (targetOperand.kind === 'Copy' && this.locals[targetOperand.place.local].linear) {
                        this.push({ kind: 'Consume', place: targetOperand.place });
                    }
                    if (val.kind === 'Copy' && this.locals[val.place.local].linear) {
                        this.push({ kind: 'Consume', place: val.place });
                    }
                    break;
                }
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

                this.terminate({ kind: 'Goto', target: bodyB });

                this.loopStack.push({ breakTo: afterB, continueTo: condB });
                this.enterBlock(bodyB);
                for (const s of stmt.body) this.lowerStatement(s);
                this.loopStack.pop();
                this.terminate({ kind: 'Goto', target: condB });

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
                break;

            case 'SpawnStatement':
                for (const s of stmt.body) this.lowerStatement(s);
                break;

            case 'EntangleStatement':
                break;

            case 'AsmStatement': {
                const p = this.materialize(
                    { kind: 'Asm', template: stmt.template }, 'void', stmt.line, stmt.column);
                break;
            }

            default:
                throw new Error(`MIR Error: unsupported statement '${(stmt as any).type}'`);
        }
    }


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

        const last = this.blocks[this.currentBlock];
        if (last.terminator === null) {
            last.terminator = (returnTy && returnTy !== 'void')
                ? { kind: 'Unreachable' }
                : { kind: 'Return', value: null };
        }

        return { owner, locals: this.locals, blocks: this.blocks, params: paramIds, returnTy, ...(layer ? { layer } : {}) };
    }

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

    public build(program: Program): MirProgram {
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
                const form = n as any;
                const fields: { name: string; ty: string }[] = [];
                for (const rank of form.ranks) {
                    for (const f of rank.fields) fields.push({ name: f.name, ty: f.type });
                }
                this.formFields.set(form.name, fields);
                if (form.packed) this.packedForms.add(form.name);
            } else if (n.type === 'FlavorDecl') {
                const flavor = n as any;
                let next = 0;
                for (const m of flavor.members) {
                    const value = m.value ?? next;
                    this.flavorMembers.set(m.name, value);
                    next = value + 1;
                }
            } else if (n.type === 'GlobalVarDecl') {
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

export function buildMir(program: Program): MirProgram {
    return new MirBuilder().build(program);
}