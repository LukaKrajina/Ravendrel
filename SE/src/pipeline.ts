import { Lexer } from '../../server/src/lexer';
import { Parser } from '../../server/src/parser';
import { SemanticAnalyzer } from '../../server/src/semantic';
import { IRGenerator } from '../../server/src/ir';

export interface PipelineResult {
    ok: boolean;
    llvmIR?: string;
    errors: string[];
}

export function compileRvdl(source: string): PipelineResult {
    try {
        const lexer = new Lexer(source);
        const parser = new Parser(lexer);
        const ast = parser.parse();
        const analyzer = new SemanticAnalyzer();
        analyzer.analyze(ast);
        if (analyzer.errors.length > 0) {
            return { ok: false, errors: analyzer.errors.map((e) => `Line ${e.line}: ${e.message}`) };
        }
        const ir = new IRGenerator();
        return { ok: true, llvmIR: ir.generate(ast), errors: [] };
    } catch (e) {
        return { ok: false, errors: [e instanceof Error ? e.message : String(e)] };
    }
}