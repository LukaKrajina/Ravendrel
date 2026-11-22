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







export interface IfStatement extends ASTNode {
    type: 'IfStatement';
    condition: Expression;
    consequent: Statement[];
    alternate: Statement[] | null;
}




export interface UnsafeBlock extends ASTNode {
    type: 'UnsafeBlock';
    body: Statement[];
}




export interface RouteCase {
    value: Expression;
    body: Statement[];
}

export interface RouteStatement extends ASTNode {
    type: 'RouteStatement';
    discriminant: Expression;
    cases: RouteCase[];
    fallback: Statement[] | null;
}




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
    isFixed?: boolean;
}





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
    isFloat?: boolean;
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





export interface ArrayLiteral extends ASTNode {
    type: 'ArrayLiteral';
    elements: Expression[];
}

export interface NullLiteral extends ASTNode {
    type: 'NullLiteral';
}

export interface Dereference extends ASTNode {
    type: 'Dereference';
    target: Expression;
}

export interface AddressOf extends ASTNode {
    type: 'AddressOf';
    target: Expression;
}










export interface NativeExpression extends ASTNode {
    type: 'NativeExpression';
    template: string;
    operands: Expression[];
}




export interface FuseArm {
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
    name: string;
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






export interface GlobalVarDecl extends ASTNode {
    type: 'GlobalVarDecl';
    name: string;
    varType: string;
    isFixed: boolean;
    value: Expression;
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
    packed?: boolean;
}





export interface FlavorMember {
    name: string;
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
    attributes?: FunctionAttribute[];
    layer?: LayerTag;
    quantum?: QuantumAttrs;
    physical?: PhysicalAttrs;
    synthetic?: boolean;
}

export interface FunctionAttribute {
    name: string;
    value: string | null;
    nums?: number[];
}








export interface PhysicalAttrs {
    coherence?: { t1: number; t2: number };
    noise?: string;
    basis?: 'X' | 'Y' | 'Z';
    decoherenceFree?: boolean;
    errorCorrection?: string;
}












export interface QuantumAttrs {
    isGate: boolean;
    undo: boolean;
    steer: boolean;
    unitary: boolean;
    measure: boolean;
}









export const ShuttleStrategy = {
    Auto: -1,
    Butterfly: 0,
    Ring: 1,
    Tree: 2,
    Grover: 3,




    C001Point: 4,
} as const;
export type ShuttleStrategyValue = (typeof ShuttleStrategy)[keyof typeof ShuttleStrategy];












export interface ShuttleTag {
    targets: number[][];
    lambda?: number;






    phase?: number;
    rounds?: number;
    strategy?: ShuttleStrategyValue;
}



















export interface TrackTag {
    observables: string[];
    mode?: number;
    slots?: number;
}








export const TrackingMode = {
    Auto: 0,
    Qnd: 1,
    Shadow: 2,
    Relabel: 3,
} as const;
export type TrackingModeValue = (typeof TrackingMode)[keyof typeof TrackingMode];














export interface LayerTag {
    time?: number;
    thread: number;
    coord: number[];
    cost?: number;
    deadline?: number;
    dual?: boolean;
    shuttle?: ShuttleTag;
    track?: TrackTag;





    unknownKeys?: { key: string; line: number; column: number }[];
}









export interface TopologyEdge {
    from: string;
    to: string;
    kind: 'stack' | 'parallel' | 'projection' | 'inversion' | 'crossing' | 'happens-before';
}

export interface TopologyCallEdge {
    caller: string;
    callee: string;
    startAt: number;
    deltaT: number;
}















export interface ShuttlePeer {
    name: string;
    coord: number[];
    logWeight: number;
    weight: number;
    distance: number;
    via: TopologyEdge['kind'];
}





export interface ShuttleLink {
    from: string;
    peers: ShuttlePeer[];
    lambda: number;
    phase: number;
    rounds: number;
    strategy: number;
    slotsPerPhase: [number, number];
}






export interface TrackingScope {
    block: string;
    observables: string[];
    mode: number;
    slots: number;
    level: number[];
    window: [number, number];
}

export interface Topology {
    shape: { time: number; thread: number; coord: number[] };
    blocks: { name: string; layer: LayerTag }[];
    edges: TopologyEdge[];
    callGraph: TopologyCallEdge[];
    shuttleLinks: ShuttleLink[];
    trackingScopes: TrackingScope[];
}

export interface ReturnStatement extends ASTNode {
    type: 'ReturnStatement';
    argument: Expression;
    isVoid?: boolean;
}





export interface SpawnStatement extends ASTNode {
    type: 'SpawnStatement';
    body: Statement[];
}





export interface EntangleStatement extends ASTNode {
    type: 'EntangleStatement';
    left: Expression;
    right: Expression;
}






export interface AsmStatement extends ASTNode {
    type: 'AsmStatement';
    template: string;
}