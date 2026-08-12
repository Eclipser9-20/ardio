// The compiler driver: source text in, AVR assembly out.
//
// The pipeline is lex -> parse -> analyse -> generate. Preprocessing and .ino
// sketch handling happen before this, in the build stage, so that compile_avr()
// can be handed either a sketch or a plain translation unit.

#include "ardio/avr/compiler.h"

#include "ardio/avr/codegen.h"
#include "ardio/avr/parser.h"
#include "ardio/avr/peephole.h"
#include "ardio/avr/preprocess.h"
#include "ardio/avr/sema.h"
#include "ardio/avr/token.h"

#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

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

// Also implemented in codegen_expr.cpp: the side table recording which globals
// were placed in flash, and under which label. A Type has no room for that
// bit, so residency travels beside the generator rather than inside the AST.
void set_flash_global(const std::string& name, const std::string& label);
void clear_flash_globals();

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

    // A generated table can be thousands of characters on one line; echoing all
    // of it buries the diagnostic rather than illustrating it.
    constexpr size_t kMaxEcho = 100;
    if (text.size() > kMaxEcho)
        text = text.substr(0, kMaxEcho) + " ... (" +
               std::to_string(text.size() - kMaxEcho) + " more characters)";

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

// The ATmega328P's SRAM is 0x0100..0x08FF. Globals are packed upwards from the
// bottom while the stack grows downwards from RAMEND, so they have to be kept
// clear of each other: this is how much room is left for the stack, calls and
// locals. It is a fixed budget rather than a guess at the program's real
// depth, but a fixed budget that is checked beats silently laying a table over
// the stack and corrupting it at run time.
constexpr int kSramStart = 0x0100;
constexpr int kSramEnd   = 0x0900;    // one past the last byte
constexpr int kStackReserve = 256;

// Appends `size` little-endian bytes of `value`.
void append_scalar(std::vector<int>& bytes, long value, int size) {
    for (int i = 0; i < size; ++i) bytes.push_back(static_cast<int>((value >> (8 * i)) & 0xFF));
}

// Folds a global's initialiser into the exact byte image the variable should
// hold when the entry point runs -- the .data section a linker would normally
// emit. Braced initialisers and string literals are laid out element by
// element in address order, and anything the source left out is zero, as C
// promises. Returns false if some part of it is not a compile-time constant.
bool fold_initialiser(const Expr* e, const TypePtr& type,
                      const std::map<std::string, long>& known,
                      std::vector<int>& bytes) {
    const int total = type ? type->size() : 2;
    if (type && type->kind == TypeKind::Array) {
        const TypePtr& elem = type->pointee;
        const int elem_size = elem ? elem->size() : 1;
        if (elem_size < 1) return false;

        if (e && e->kind == ExprKind::StringLiteral) {
            for (long i = 0; i < type->array_length; ++i) {
                const std::string& s = e->str_value;
                long ch = i < static_cast<long>(s.size())
                              ? static_cast<unsigned char>(s[static_cast<size_t>(i)])
                              : 0;
                append_scalar(bytes, ch, elem_size);
            }
            return true;
        }
        if (e && e->kind != ExprKind::InitList) return false;
        for (long i = 0; i < type->array_length; ++i) {
            const Expr* sub = nullptr;
            if (e && i < static_cast<long>(e->args.size()))
                sub = e->args[static_cast<size_t>(i)].get();
            if (!fold_initialiser(sub, elem, known, bytes)) return false;
        }
        return true;
    }

    if (!e) {                                   // left out: zero fill
        append_scalar(bytes, 0, total);
        return true;
    }
    if (e->kind == ExprKind::InitList) {        // `int x = { 1 }`
        const Expr* sub = e->args.empty() ? nullptr : e->args[0].get();
        return fold_initialiser(sub, type, known, bytes);
    }
    long v = 0;
    if (!fold_constant(*e, known, v)) return false;
    append_scalar(bytes, v, total);
    return true;
}

// ------------------------------------------------------- flash placement ---
//
// A global array whose bytes never change is dead weight in SRAM: the startup
// copy writes it there byte by byte, paying flash for the copy code and SRAM
// for the result, when the bytes were already sitting in flash to begin with.
// Placing such a table in flash and reading it with LPM costs nothing but the
// LPM sequence at each read.
//
// Placement is only safe when the compiler can see that every use is a
// fully-subscripted read. Two things disqualify a table:
//
//   * a store through it, `t[i] = x` -- flash cannot be written at run time,
//     and silently dropping the store would be a miscompile;
//   * any other mention of the name at all -- `&t`, `t` passed to a function,
//     `t[i]` where that yields a row rather than an element. Each of those
//     wants a data address, and the address of a flash table is not one: it
//     names bytes that LD cannot reach.
//
// The second rule subsumes the first and is deliberately blunt. Anything it
// turns down simply keeps the existing SRAM behaviour, so a table that cannot
// move is never an error, only a missed saving.

// The file-scope arrays that were written `const`.
//
// Const-ness does not survive parsing: the qualifier is recognised as part of
// a declaration's type words and then dropped, and Type has no bit to keep it
// in. Rather than change the shape of the AST, the qualifier is recovered from
// the token stream the parser was handed -- the declaration `const T name[N]`
// is unambiguous in tokens, and only file scope is looked at, so nothing
// inside a function body can be mistaken for one.
//
// Const-ness alone never justifies moving a table; the use analysis below
// still has to agree. It is required as well because it is the programmer's
// own statement that the bytes are fixed, which keeps a plain mutable array
// that merely happens to go unwritten in the SRAM it was declared to occupy.
std::set<std::string> const_declared_arrays(const std::vector<Token>& tokens) {
    std::set<std::string> names;
    int depth = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        if (t.kind == Tok::Punct) {
            if (t.text == "{" || t.text == "(") ++depth;
            else if (t.text == "}" || t.text == ")") --depth;
            continue;
        }
        if (depth != 0 || t.text != "const") continue;

        for (size_t j = i + 1; j + 1 < tokens.size(); ++j) {
            const Token& u = tokens[j];
            if (u.kind == Tok::Punct && (u.text == ";" || u.text == "{" ||
                                         u.text == "(" || u.text == "="))
                break;
            if (u.kind != Tok::Identifier) continue;
            if (tokens[j + 1].kind == Tok::Punct && tokens[j + 1].text == "[") {
                names.insert(u.text);
                break;
            }
        }
    }
    return names;
}

// The identifier a chain of subscripts is rooted at, or null if the base is
// not a plain name.
const Expr* subscript_root(const Expr& e) {
    const Expr* p = &e;
    while (p->kind == ExprKind::Index) {
        if (!p->lhs) return nullptr;
        p = p->lhs.get();
    }
    return p->kind == ExprKind::Identifier ? p : nullptr;
}

// Records the root identifier of every subscript chain that is read as a whole
// element. `suppress` marks a position where the value is not merely read --
// an assignment target, the operand of '&' or of ++/-- -- so the chain
// underneath it stays unrecorded and its root is disqualified below.
void collect_element_reads(const Expr* e, std::set<const Expr*>& reads, bool suppress) {
    if (!e) return;

    if (e->kind == ExprKind::Index && !suppress && e->type &&
        e->type->kind != TypeKind::Array) {
        if (const Expr* root = subscript_root(*e)) reads.insert(root);
    }

    bool suppress_lhs = e->kind == ExprKind::Assign ||
                        (e->kind == ExprKind::Unary &&
                         (e->op == "&" || e->op == "++" || e->op == "--"));

    collect_element_reads(e->lhs.get(), reads, suppress_lhs);
    collect_element_reads(e->rhs.get(), reads, false);
    collect_element_reads(e->third.get(), reads, false);
    for (const ExprPtr& a : e->args) collect_element_reads(a.get(), reads, false);
}

// Every name mentioned outside the recorded reads. Any global appearing here
// needs a real data address and therefore cannot move to flash.
void collect_other_mentions(const Expr* e, const std::set<const Expr*>& reads,
                            std::set<std::string>& out) {
    if (!e) return;
    if (e->kind == ExprKind::Identifier && !reads.count(e)) out.insert(e->name);
    collect_other_mentions(e->lhs.get(), reads, out);
    collect_other_mentions(e->rhs.get(), reads, out);
    collect_other_mentions(e->third.get(), reads, out);
    for (const ExprPtr& a : e->args) collect_other_mentions(a.get(), reads, out);
}

void gather_exprs_stmt(const Stmt* s, std::vector<const Expr*>& out) {
    if (!s) return;
    out.push_back(s->expr.get());
    out.push_back(s->var_init.get());
    out.push_back(s->step.get());
    for (const ExprPtr& a : s->ctor_args) out.push_back(a.get());
    for (const StmtPtr& c : s->body) gather_exprs_stmt(c.get(), out);
    gather_exprs_stmt(s->then_branch.get(), out);
    gather_exprs_stmt(s->else_branch.get(), out);
    gather_exprs_stmt(s->init.get(), out);
}

// How every global name is used across the program: `needs_sram` holds the
// names mentioned anywhere a data address is required, `element_reads` the
// names subscripted down to a single element. A table qualifies for flash when
// it appears in the second and not the first -- the second matters because a
// table nothing ever reads has no LPM site to justify moving it, and moving it
// would only change where an unused image sits.
struct GlobalUses {
    std::set<std::string> needs_sram;
    std::set<std::string> element_reads;
};

GlobalUses classify_global_uses(const Program& program) {
    std::vector<const Expr*> roots;
    for (const Function& f : program.functions) gather_exprs_stmt(f.body.get(), roots);
    for (const ClassDecl& c : program.classes)
        for (const Function& m : c.methods) gather_exprs_stmt(m.body.get(), roots);
    for (const Global& g : program.globals) {
        roots.push_back(g.init.get());
        for (const ExprPtr& a : g.ctor_args) roots.push_back(a.get());
    }

    GlobalUses uses;
    for (const Expr* e : roots) {
        if (!e) continue;
        std::set<const Expr*> reads;
        collect_element_reads(e, reads, false);
        for (const Expr* r : reads) uses.element_reads.insert(r->name);
        collect_other_mentions(e, reads, uses.needs_sram);
    }
    return uses;
}

// The scalar an array bottoms out in, or null if it is not an array of
// scalars all the way down.
const Type* array_element_scalar(const Type* t) {
    if (!t || t->kind != TypeKind::Array) return nullptr;
    while (t->kind == TypeKind::Array) {
        if (!t->pointee) return nullptr;
        t = t->pointee.get();
    }
    return t->kind == TypeKind::Class ? nullptr : t;
}

// Whether a global's shape allows flash placement, ignoring how it is used.
bool shape_allows_flash(const Global& g) {
    if (!g.init || !g.type || g.type->kind != TypeKind::Array) return false;
    if (g.type->array_length <= 0) return false;
    const Type* elem = array_element_scalar(g.type.get());
    if (!elem) return false;
    const int size = elem->size();
    return size == 1 || size == 2 || size == 4;   // what LPM reads back
}

bool has_function(const Program& p, const std::string& name);

// ---------------------------------------------------------- reachability ---
//
// Every function in every included header would otherwise be emitted, whether
// or not anything calls it -- an empty sketch that includes <Arduino.h> came
// out at 4.5 KB of unreachable code. avr-gcc solves this by putting each
// function in its own section and letting the linker garbage-collect them;
// ardio has no linker, so it walks the call graph from the entry points and
// emits only what is reachable.

void collect_calls_expr(const Expr* e, std::set<std::string>& out);

void collect_calls_stmt(const Stmt* s, std::set<std::string>& out) {
    if (!s) return;
    collect_calls_expr(s->expr.get(), out);
    collect_calls_expr(s->var_init.get(), out);
    collect_calls_expr(s->step.get(), out);
    for (const ExprPtr& a : s->ctor_args) collect_calls_expr(a.get(), out);
    for (const StmtPtr& c : s->body) collect_calls_stmt(c.get(), out);
    collect_calls_stmt(s->then_branch.get(), out);
    collect_calls_stmt(s->else_branch.get(), out);
    collect_calls_stmt(s->init.get(), out);
}

void collect_calls_expr(const Expr* e, std::set<std::string>& out) {
    if (!e) return;
    if (e->kind == ExprKind::Call && !e->name.empty()) {
        // A method call carries its object in lhs; the label the generator
        // emits is ClassName__method.
        std::string class_name;
        if (e->lhs && e->lhs->type) {
            const TypePtr& t = e->lhs->type;
            if (t->kind == TypeKind::Class) class_name = t->class_name;
            else if (t->kind == TypeKind::Pointer && t->pointee &&
                     t->pointee->kind == TypeKind::Class)
                class_name = t->pointee->class_name;
        }
        out.insert(class_name.empty() ? e->name : class_name + "__" + e->name);
    }
    collect_calls_expr(e->lhs.get(), out);
    collect_calls_expr(e->rhs.get(), out);
    collect_calls_expr(e->third.get(), out);
    for (const ExprPtr& a : e->args) collect_calls_expr(a.get(), out);
}

// Names of everything reachable from the entry points, as the generator would
// label them.
std::set<std::string> reachable_symbols(const Program& program) {
    std::map<std::string, std::set<std::string>> calls;   // label -> callees
    for (const Function& f : program.functions)
        if (f.body) collect_calls_stmt(f.body.get(), calls[f.name]);
    for (const ClassDecl& c : program.classes)
        for (const Function& m : c.methods)
            if (m.body)
                collect_calls_stmt(m.body.get(),
                                   calls[c.name + "__" +
                                         (m.is_constructor ? "ctor" : m.name)]);

    std::set<std::string> reached;
    std::vector<std::string> worklist;
    auto add = [&](const std::string& name) {
        if (reached.insert(name).second) worklist.push_back(name);
    };

    for (const char* root : {"setup", "loop", "main"})
        if (has_function(program, root)) add(root);

    // A global of class type runs its constructor before the entry point.
    for (const Global& g : program.globals) {
        if (g.type && g.type->kind == TypeKind::Class)
            add(g.type->class_name + "__ctor");
        collect_calls_expr(g.init.get(), reached);
        for (const ExprPtr& a : g.ctor_args) collect_calls_expr(a.get(), reached);
    }
    for (const std::string& n : reached) worklist.push_back(n);

    while (!worklist.empty()) {
        std::string name = worklist.back();
        worklist.pop_back();
        auto it = calls.find(name);
        if (it == calls.end()) continue;
        for (const std::string& callee : it->second) add(callee);
    }
    return reached;
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

    // Decide which read-only tables can live in flash, before a single byte of
    // SRAM is handed out. The images are folded here with the same walk the
    // startup code uses below, so a table whose initialiser turns out not to
    // be constant simply keeps the old SRAM path and the old diagnostic.
    clear_flash_globals();
    std::map<std::string, std::vector<int>> flash_images;
    {
        const GlobalUses uses = classify_global_uses(parsed.program);
        const std::set<std::string> declared_const = const_declared_arrays(tokens);
        std::map<std::string, long> known;
        for (const Global& g : parsed.program.globals) {
            std::vector<int> image;
            if (!g.init || !fold_initialiser(g.init.get(), g.type, known, image)) continue;
            if (g.type && g.type->kind != TypeKind::Array && !image.empty()) {
                long v = 0;
                for (size_t i = image.size(); i-- > 0;) v = (v << 8) | image[i];
                known[g.name] = v;
                continue;
            }
            if (image.empty()) continue;
            if (!declared_const.count(g.name)) continue;      // not declared const
            if (uses.needs_sram.count(g.name)) continue;      // wants an address
            if (!uses.element_reads.count(g.name)) continue;  // never read at all
            if (!shape_allows_flash(g)) continue;
            set_flash_global(g.name, "__flash_" + g.name);
            flash_images[g.name] = std::move(image);
        }
    }

    // Reserve SRAM for globals before any code refers to them.
    for (const Global& g : parsed.program.globals) {
        if (flash_images.count(g.name)) continue;      // its bytes are in flash
        int size = g.type ? g.type->size() : 2;
        if (size < 1) size = 1;
        const int limit = kSramEnd - kStackReserve;
        if (gen.next_global_address + size > limit) {
            const int used = gen.next_global_address - kSramStart;
            result.error = "line " + std::to_string(g.line) + ": '" + g.name + "' needs " +
                           std::to_string(size) + " bytes of SRAM, but only " +
                           std::to_string(limit - gen.next_global_address) +
                           " of the " + std::to_string(limit - kSramStart) +
                           " bytes available to globals are left (" + std::to_string(used) +
                           " already in use, and " + std::to_string(kStackReserve) +
                           " bytes are reserved for the stack). This chip has " +
                           std::to_string(kSramEnd - kSramStart) +
                           " bytes of SRAM in total.";
            return result;
        }
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
    int loaded = -1;                      // byte currently in r24, or -1 if unknown
    for (const Global& g : parsed.program.globals) {
        if (!g.init) continue;
        if (flash_images.count(g.name)) continue;      // emitted after the code
        int addr = gen.global_address(g.name);
        if (addr < 0) continue;

        std::vector<int> image;
        if (!fold_initialiser(g.init.get(), g.type, constant_globals, image)) {
            result.error = "line " + std::to_string(g.line) + ": initialiser for '" +
                           g.name + "' is not a constant. Globals are stored before " +
                           "the program starts, so their initialisers must be " +
                           "computable at compile time.";
            return result;
        }
        // Only a scalar has a value later initialisers can name.
        if (g.type && g.type->kind != TypeKind::Array && !image.empty()) {
            long v = 0;
            for (size_t i = image.size(); i-- > 0;) v = (v << 8) | image[i];
            constant_globals[g.name] = v;
        }
        for (size_t i = 0; i < image.size(); ++i) {
            // A table repeats byte values constantly; reloading r24 only when
            // the value actually changes roughly halves the setup code.
            if (image[i] != loaded) {
                gen.emit("    ldi  r24, " + std::to_string(image[i]));
                loaded = image[i];
            }
            gen.emit("    sts  " + std::to_string(addr + static_cast<int>(i)) + ", r24");
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

    const std::set<std::string> reachable = reachable_symbols(parsed.program);

    for (const ClassDecl& c : parsed.program.classes) {
        for (const Function& m : c.methods) {
            std::string label = c.name + "__" + (m.is_constructor ? "ctor" : m.name);
            if (!reachable.count(label)) continue;   // nothing calls it
            gen.gen_class_method(c, m);
            if (gen.failed()) {
                result.error = gen.error;
                return result;
            }
        }
    }

    for (const Function& f : parsed.program.functions) {
        if (f.body && !reachable.count(f.name)) continue;   // nothing calls it
        gen.gen_function(f);
        if (gen.failed()) {
            result.error = gen.error;
            return result;
        }
    }

    // The read-only tables, laid down after the last instruction so that no
    // execution path can run into them. Labels name word addresses, and the
    // assembler rounds up to a word before recording one, so each table starts
    // on a word boundary and its byte address is exactly twice its label --
    // which is the conversion the LPM sequences apply.
    for (const Global& g : parsed.program.globals) {
        auto it = flash_images.find(g.name);
        if (it == flash_images.end()) continue;
        const std::vector<int>& image = it->second;

        gen.emit("");
        gen.emit("; " + g.name + ": read-only, kept in flash and read with LPM (" +
                 std::to_string(image.size()) + " bytes of SRAM saved)");
        gen.emit_label("__flash_" + g.name);
        constexpr size_t kPerLine = 16;
        for (size_t i = 0; i < image.size(); i += kPerLine) {
            std::string line = "    .byte ";
            for (size_t j = i; j < image.size() && j < i + kPerLine; ++j) {
                if (j > i) line += ", ";
                line += std::to_string(image[j] & 0xFF);
            }
            gen.emit(line);
        }
    }

    result.ok = true;
    result.assembly = optimise_assembly(gen.out);
    return result;
}

} // namespace ardio
