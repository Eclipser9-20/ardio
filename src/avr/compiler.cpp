// The compiler driver: source text in, AVR assembly out.
//
// The pipeline is lex -> parse -> analyse -> generate. Preprocessing and .ino
// sketch handling happen before this, in the build stage, so that compile_avr()
// can be handed either a sketch or a plain translation unit.

#include "ardio/avr/compiler.h"

#include "ardio/avr/codegen.h"
#include "ardio/avr/parser.h"
#include "ardio/avr/preprocess.h"
#include "ardio/avr/sema.h"
#include "ardio/avr/token.h"

#include <map>

namespace ardio {

// Implemented in codegen_class.cpp: emits constructor calls for globals of
// class type. Declared here rather than in codegen.h so that the class
// generator owns its own surface.
void gen_global_object_init(CodeGen& gen, const Program& program);

// Implemented in codegen_expr.cpp: publishes a class's field offsets to the
// generator, which needs them for obj.field because a Type carries only the
// class name.
void set_class_layout(const ClassDecl& decl);
void clear_class_layouts();

namespace {

// Preprocessor directives are not handled by this compiler yet: there is a
// full preprocessor in tree, but ardio's own Arduino headers use C++ features
// the code generator cannot compile, so including them would fail later and
// less clearly. Directives are blanked here, preserving line numbers, and a
// sketch that actually depended on a header then fails with an honest
// "undeclared identifier" naming the symbol it was missing.
std::string blank_directives(std::string_view source) {
    std::string out;
    out.reserve(source.size());
    size_t i = 0;
    while (i <= source.size()) {
        size_t end = source.find('\n', i);
        bool last = end == std::string_view::npos;
        if (last) end = source.size();
        std::string_view line = source.substr(i, end - i);

        size_t first = line.find_first_not_of(" \t");
        bool directive = first != std::string_view::npos && line[first] == '#';
        if (!directive) out.append(line);
        out.push_back('\n');

        if (last) break;
        i = end + 1;
    }
    return out;
}

// Folds a global's initialiser to a constant. Globals are stored before the
// entry point runs, standing in for the .data image a linker would normally
// produce, so an initialiser has to be computable now. Anything that is not
// becomes a diagnostic rather than a silent zero.
bool fold_constant(const Expr& e, const std::map<std::string, long>& known, long& out) {
    switch (e.kind) {
    case ExprKind::IntLiteral:
        out = e.int_value;
        return true;
    case ExprKind::Identifier: {
        auto it = known.find(e.name);
        if (it == known.end()) return false;
        out = it->second;
        return true;
    }
    case ExprKind::Unary: {
        long v = 0;
        if (!e.lhs || !fold_constant(*e.lhs, known, v)) return false;
        if (e.op == "-") { out = -v; return true; }
        if (e.op == "+") { out = v;  return true; }
        if (e.op == "~") { out = ~v; return true; }
        if (e.op == "!") { out = !v; return true; }
        return false;
    }
    case ExprKind::Binary: {
        long a = 0, b = 0;
        if (!e.lhs || !e.rhs) return false;
        if (!fold_constant(*e.lhs, known, a)) return false;
        if (!fold_constant(*e.rhs, known, b)) return false;
        if (e.op == "+")  { out = a + b;  return true; }
        if (e.op == "-")  { out = a - b;  return true; }
        if (e.op == "*")  { out = a * b;  return true; }
        if (e.op == "/")  { if (b == 0) return false; out = a / b; return true; }
        if (e.op == "%")  { if (b == 0) return false; out = a % b; return true; }
        if (e.op == "<<") { out = a << b; return true; }
        if (e.op == ">>") { out = a >> b; return true; }
        if (e.op == "&")  { out = a & b;  return true; }
        if (e.op == "|")  { out = a | b;  return true; }
        if (e.op == "^")  { out = a ^ b;  return true; }
        return false;
    }
    case ExprKind::Cast:
        return e.lhs && fold_constant(*e.lhs, known, out);
    default:
        return false;
    }
}

bool has_function(const Program& p, const std::string& name) {
    for (const Function& f : p.functions)
        if (f.name == name && f.body) return true;
    return false;
}

} // namespace

CompileResult compile_avr(std::string_view source,
                          const std::vector<std::string>& include_paths) {
    PreprocessResult pp = preprocess(std::string(source), include_paths);
    if (!pp.ok) {
        CompileResult result;
        result.error = pp.error;
        return result;
    }
    return compile_avr(pp.text);
}

CompileResult compile_avr(std::string_view source) {
    CompileResult result;

    std::vector<Token> tokens = tokenize(blank_directives(source));

    ParseResult parsed = parse(tokens);
    if (!parsed.ok) {
        result.error = parsed.error;
        return result;
    }

    SemaResult analysed = analyse(parsed.program);
    if (!analysed.ok) {
        result.error = analysed.error;
        return result;
    }

    CodeGen gen;

    // Field offsets are computed by semantic analysis; hand them to the
    // generator before any code refers to a member.
    clear_class_layouts();
    for (const ClassDecl& c : parsed.program.classes) set_class_layout(c);

    // Reserve SRAM for globals before any code refers to them.
    for (const Global& g : parsed.program.globals) {
        int size = g.type ? g.type->size() : 2;
        if (size < 1) size = 1;
        gen.add_global(g.name, size);
    }

    const bool arduino_style = has_function(parsed.program, "setup") &&
                               has_function(parsed.program, "loop");
    const bool freestanding = has_function(parsed.program, "main");

    if (!arduino_style && !freestanding) {
        result.error = "no entry point: define either setup() and loop(), or main()";
        return result;
    }

    gen.emit("; generated by ardio");
    gen.emit("");
    gen.emit("    clr  r1                 ; r1 is permanently zero");
    gen.emit("    out  0x3F, r1           ; clear SREG");
    gen.emit("    ldi  r28, 0xFF          ; stack pointer to RAMEND (0x08FF)");
    gen.emit("    out  0x3D, r28");
    gen.emit("    ldi  r28, 0x08");
    gen.emit("    out  0x3E, r28");

    // Globals with constant initialisers are stored before the entry point
    // runs, standing in for the .data copy a linker would normally perform.
    std::map<std::string, long> constant_globals;
    for (const Global& g : parsed.program.globals) {
        if (!g.init) continue;
        int size = g.type ? g.type->size() : 2;
        int addr = gen.global_address(g.name);
        if (addr < 0) continue;

        long v = 0;
        if (!fold_constant(*g.init, constant_globals, v)) {
            result.error = "line " + std::to_string(g.line) + ": initialiser for '" +
                           g.name + "' is not a constant. Globals are stored before " +
                           "the program starts, so their initialisers must be " +
                           "computable at compile time.";
            return result;
        }
        constant_globals[g.name] = v;
        gen.emit("    ldi  r24, " + std::to_string(v & 0xFF));
        gen.emit("    sts  " + std::to_string(addr) + ", r24");
        if (size >= 2) {
            gen.emit("    ldi  r24, " + std::to_string((v >> 8) & 0xFF));
            gen.emit("    sts  " + std::to_string(addr + 1) + ", r24");
        }
    }

    gen_global_object_init(gen, parsed.program);
    if (gen.failed()) {
        result.error = gen.error;
        return result;
    }

    if (arduino_style) {
        gen.emit("    call setup");
        gen.emit_label(".Lmainloop");
        gen.emit("    call loop");
        gen.emit("    rjmp .Lmainloop");
    } else {
        gen.emit("    call main");
        gen.emit_label(".Lhalt");
        gen.emit("    rjmp .Lhalt");
    }

    for (const ClassDecl& c : parsed.program.classes) {
        for (const Function& m : c.methods) {
            gen.gen_class_method(c, m);
            if (gen.failed()) {
                result.error = gen.error;
                return result;
            }
        }
    }

    for (const Function& f : parsed.program.functions) {
        gen.gen_function(f);
        if (gen.failed()) {
            result.error = gen.error;
            return result;
        }
    }

    result.ok = true;
    result.assembly = gen.out;
    return result;
}

} // namespace ardio
