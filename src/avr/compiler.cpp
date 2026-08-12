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

#include <cctype>
#include <map>
#include <string>

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

// Blanks preprocessor directives, preserving line numbers.
//
// This is the fallback path only. Normally compile_avr() is handed include
// paths and runs the real preprocessor, so #include and #define work. When
// preprocessing cannot even be attempted -- a header that is nowhere on the
// include path, a malformed directive -- the source is compiled once more with
// directives blanked, so a sketch that named a header but never used anything
// from it still builds. If that attempt fails too, the preprocessor's own
// error leads the diagnostic, because it names the header.
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

// The names of every header the source #includes directly, in source order.
// Used only to sharpen a diagnostic, so conditionals are not evaluated here:
// a header is reported on only if it genuinely fails to compile on its own.
std::vector<std::string> included_headers(std::string_view source) {
    std::vector<std::string> names;
    size_t i = 0;
    while (i <= source.size()) {
        size_t end = source.find('\n', i);
        if (end == std::string_view::npos) end = source.size();
        std::string_view line = source.substr(i, end - i);
        i = end + 1;

        size_t p = line.find_first_not_of(" \t");
        if (p == std::string_view::npos || line[p] != '#') continue;
        p = line.find_first_not_of(" \t", p + 1);
        if (p == std::string_view::npos) continue;
        if (line.compare(p, 7, "include") != 0) continue;
        p = line.find_first_not_of(" \t", p + 7);
        if (p == std::string_view::npos) continue;

        char close = line[p] == '<' ? '>' : (line[p] == '"' ? '"' : '\0');
        if (close == '\0') continue;
        size_t stop = line.find(close, p + 1);
        if (stop == std::string_view::npos) continue;
        names.emplace_back(line.substr(p + 1, stop - p - 1));
    }
    return names;
}

// Compiles one header on its own, with a trivial entry point supplied, to find
// out whether it is the reason a sketch failed to build.
bool header_compiles(const std::string& name,
                     const std::vector<std::string>& include_paths,
                     std::string& error) {
    std::string probe = "#include <" + name + ">\nvoid setup() {}\nvoid loop() {}\n";
    PreprocessResult pp = preprocess(probe, include_paths);
    if (!pp.ok) return true;   // not found here: not this header's fault to report
    CompileResult r = compile_avr(pp.text);
    if (r.ok) return true;
    error = r.error;
    return false;
}

// Quotes the line an error points at. After preprocessing, line numbers count
// lines of the whole translation unit -- sketch plus every header pulled into
// it -- so the number alone can be misleading. Showing the line itself makes
// the diagnostic self-explanatory regardless.
std::string quote_error_line(const std::string& error, const std::string& unit) {
    const std::string tag = "line ";
    size_t p = error.find(tag);
    if (p != 0) return {};
    p += tag.size();
    size_t n = 0;
    size_t digits = 0;
    while (p + digits < error.size() &&
           std::isdigit(static_cast<unsigned char>(error[p + digits]))) {
        n = n * 10 + size_t(error[p + digits] - '0');
        ++digits;
    }
    if (digits == 0 || n == 0) return {};

    size_t start = 0;
    for (size_t i = 1; i < n; ++i) {
        start = unit.find('\n', start);
        if (start == std::string::npos) return {};
        ++start;
    }
    size_t end = unit.find('\n', start);
    std::string text = unit.substr(start, end == std::string::npos ? end : end - start);
    size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    text = text.substr(first);
    return "\n  " + std::to_string(n) + " | " + text +
           "\n(line numbers count the whole translation unit: the sketch plus "
           "every header it includes)";
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
        // Preprocessing did not even get off the ground. Try once more with
        // directives blanked, so a sketch that names a header it never actually
        // uses still builds -- but if that fails too, lead with the
        // preprocessor's message, which names the header.
        CompileResult fallback = compile_avr(source);
        if (fallback.ok) return fallback;

        CompileResult result;
        result.error = pp.error +
                       "\nardio then retried with preprocessor directives ignored, "
                       "which failed as well: " + fallback.error;
        return result;
    }

    CompileResult result = compile_avr(pp.text);
    if (result.ok) return result;

    // The unit preprocessed cleanly but would not compile. If one of the
    // headers it pulled in is the reason, say so by name: that is far more
    // useful than a line number pointing into expanded header text.
    for (const std::string& name : included_headers(source)) {
        std::string why;
        if (header_compiles(name, include_paths, why)) continue;
        result.error = "cannot compile '" + name +
                       "' yet -- ardio's compiler rejects the header itself: " + why;
        return result;
    }

    result.error += quote_error_line(result.error, pp.text);
    return result;
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
