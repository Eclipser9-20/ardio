#include "ardio/avr/parser.h"

#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ardio {
namespace {

// Thrown internally on the first syntax error and converted to a ParseResult
// at the API boundary. The message already contains the "line N: " prefix.
struct ParseError : std::runtime_error {
    explicit ParseError(std::string msg) : std::runtime_error(std::move(msg)) {}
};

bool is_type_qualifier(const std::string& w) {
    return w == "const" || w == "static" || w == "volatile" || w == "register" ||
           w == "extern" || w == "inline" || w == "constexpr" || w == "unsigned" ||
           w == "signed" || w == "short" || w == "long";
}

bool is_base_type_word(const std::string& w) {
    return w == "void" || w == "bool" || w == "char" || w == "int" || w == "long" ||
           w == "short" || w == "unsigned" || w == "signed";
}

class Parser {
public:
    Parser(const std::vector<Token>& tokens, Program& program)
        : toks_(tokens), program_(program) {}

    void parse_program() {
        while (!at_end()) {
            if (match_punct(";")) continue;
            parse_top_level(program_);
        }
    }

private:
    // ------------------------------------------------------------ cursor ---

    const Token& peek(size_t ahead = 0) const {
        size_t i = pos_ + ahead;
        return i < toks_.size() ? toks_[i] : end_token();
    }

    const Token& end_token() const {
        static const Token e{};
        return e;
    }

    bool at_end() const { return peek().kind == Tok::End; }

    size_t line() const {
        const Token& t = peek();
        if (t.kind != Tok::End || toks_.empty()) return t.line;
        // At end-of-input report the last real line rather than 0.
        return toks_[toks_.size() - 1].line;
    }

    const Token& advance() {
        const Token& t = peek();
        if (pos_ < toks_.size()) ++pos_;
        return t;
    }

    bool is_punct(const std::string& p, size_t ahead = 0) const {
        const Token& t = peek(ahead);
        return t.kind == Tok::Punct && t.text == p;
    }

    bool is_word(const std::string& w, size_t ahead = 0) const {
        const Token& t = peek(ahead);
        return (t.kind == Tok::Keyword || t.kind == Tok::Identifier) && t.text == w;
    }

    bool match_punct(const std::string& p) {
        if (!is_punct(p)) return false;
        ++pos_;
        return true;
    }

    bool match_word(const std::string& w) {
        if (!is_word(w)) return false;
        ++pos_;
        return true;
    }

    void expect_punct(const std::string& p, const std::string& what) {
        if (!match_punct(p)) error("expected '" + p + "' " + what);
    }

    [[noreturn]] void error(const std::string& what) const {
        throw ParseError("line " + std::to_string(line()) + ": " + what);
    }

    std::string expect_identifier(const std::string& what) {
        const Token& t = peek();
        if (t.kind != Tok::Identifier) error("expected " + what);
        ++pos_;
        return t.text;
    }

    // ------------------------------------------------------------- types ---

    bool starts_type(size_t ahead = 0) const {
        const Token& t = peek(ahead);
        if (t.kind == Tok::Keyword && (is_base_type_word(t.text) || is_type_qualifier(t.text)))
            return true;
        if (t.kind == Tok::Identifier) {
            if (is_base_type_word(t.text) || is_type_qualifier(t.text)) return true;
            return classes_.count(t.text) != 0;
        }
        return false;
    }

    // True when the tokens at the cursor begin a declaration rather than an
    // expression: a type followed by a declarator name.
    bool starts_declaration() const {
        if (!starts_type()) return false;
        const Token& t = peek();
        bool builtin = is_base_type_word(t.text) || is_type_qualifier(t.text);
        if (builtin) return true;
        // Class type: require "Name ident", "Name *", or "Name &" so that a bare
        // "Name(...)" call expression is not mistaken for a declaration.
        size_t k = 1;
        while (is_punct("*", k) || is_punct("&", k)) ++k;
        if (k > 1) return peek(k).kind == Tok::Identifier;
        return peek(1).kind == Tok::Identifier;
    }

    TypePtr parse_type() {
        bool seen_unsigned = false, seen_signed = false, seen_long = false;
        bool seen_short = false, seen_base = false;
        TypePtr base;
        std::string class_name;

        for (;;) {
            const Token& t = peek();
            if (t.kind != Tok::Keyword && t.kind != Tok::Identifier) break;
            const std::string& w = t.text;
            if (w == "const" || w == "volatile" || w == "static" || w == "register" ||
                w == "extern" || w == "inline" || w == "constexpr") {
                ++pos_;
                continue;
            }
            if (w == "unsigned") { seen_unsigned = true; ++pos_; continue; }
            if (w == "signed")   { seen_signed = true;   ++pos_; continue; }
            if (w == "long")     { seen_long = true;     ++pos_; continue; }
            if (w == "short")    { seen_short = true;    ++pos_; continue; }
            if (!seen_base && (w == "void" || w == "bool" || w == "char" || w == "int")) {
                seen_base = true;
                class_name = w;
                ++pos_;
                continue;
            }
            if (!seen_base && !seen_unsigned && !seen_signed && !seen_long && !seen_short &&
                classes_.count(w)) {
                seen_base = true;
                class_name = w;
                base = make_type(TypeKind::Class);
                base->class_name = w;
                ++pos_;
                continue;
            }
            break;
        }

        if (!base) {
            if (!seen_base && !seen_unsigned && !seen_signed && !seen_long && !seen_short)
                error("expected a type name");
            bool uns = seen_unsigned;
            if (class_name == "void") {
                base = make_type(TypeKind::Void);
            } else if (class_name == "bool") {
                base = make_type(TypeKind::Bool);
            } else if (class_name == "char") {
                base = make_type(TypeKind::Char);
                base->is_signed = !uns;
            } else if (seen_long) {
                base = make_type(uns ? TypeKind::ULong : TypeKind::Long);
                base->is_signed = !uns;
            } else {
                base = make_type(uns ? TypeKind::UInt : TypeKind::Int);
                base->is_signed = !uns;
            }
        }

        // Pointers; references are modelled as pointers.
        for (;;) {
            if (match_punct("*")) {
                base = make_pointer(base);
            } else if (is_punct("&") && peek(1).kind == Tok::Identifier) {
                ++pos_;
                base = make_pointer(base);
            } else {
                break;
            }
        }
        return base;
    }

    // Applies any trailing "[N]" suffixes to a declarator's type.
    TypePtr parse_array_suffix(TypePtr base) {
        std::vector<long> dims;
        while (is_punct("[")) {
            ++pos_;
            long n = 0;
            if (!is_punct("]")) {
                ExprPtr size = parse_assignment();
                if (size->kind == ExprKind::IntLiteral) n = size->int_value;
            }
            expect_punct("]", "after array length");
            dims.push_back(n);
        }
        for (size_t i = dims.size(); i-- > 0;) base = make_array(base, dims[i]);
        return base;
    }

    // -------------------------------------------------------- top level ---

    void parse_top_level(Program& program) {
        if (is_word("class") || is_word("struct")) {
            // A forward declaration ("class Foo;") is recorded as a known name.
            if (peek(1).kind == Tok::Identifier && is_punct(";", 2)) {
                classes_.insert(peek(1).text);
                pos_ += 3;
                return;
            }
            parse_class(program);
            return;
        }
        if (is_word("enum")) {
            parse_enum(program);
            return;
        }
        if (is_word("namespace")) {
            // Namespaces are flattened: the body is parsed into the same Program.
            ++pos_;
            if (peek().kind == Tok::Identifier) ++pos_;
            expect_punct("{", "to open a namespace");
            while (!is_punct("}")) {
                if (at_end()) error("expected '}' to close namespace");
                parse_top_level(program);
            }
            expect_punct("}", "to close a namespace");
            return;
        }
        if (is_word("using") || is_word("typedef")) {
            while (!at_end() && !is_punct(";")) ++pos_;
            expect_punct(";", "after declaration");
            return;
        }
        parse_declaration(program, nullptr);
    }

    // Parses "type declarator[, declarator]* ;" or a function at file scope.
    // When owner is non-null the declaration is a class member.
    void parse_declaration(Program& program, ClassDecl* owner) {
        size_t decl_line = line();

        // Constructor / destructor inside a class body.
        if (owner) {
            bool dtor = is_punct("~") && peek(1).kind == Tok::Identifier &&
                        peek(1).text == owner->name && is_punct("(", 2);
            bool ctor = peek().kind == Tok::Identifier && peek().text == owner->name &&
                        is_punct("(", 1);
            if (dtor || ctor) {
                if (dtor) ++pos_;
                std::string name = advance().text;
                Function fn;
                fn.name = dtor ? "~" + name : name;
                fn.owner_class = owner->name;
                fn.is_constructor = !dtor;
                fn.return_type = make_type(TypeKind::Void);
                fn.line = decl_line;
                expect_punct("(", "to open a parameter list");
                fn.params = parse_params();
                skip_member_init_list();
                finish_function_body(fn);
                owner->methods.push_back(std::move(fn));
                return;
            }
        }

        TypePtr base = parse_type();

        bool first = true;
        for (;;) {
            TypePtr type = base;
            while (match_punct("*")) type = make_pointer(type);

            std::string name = expect_identifier("a declarator name");
            std::string qualified_class;
            if (is_punct("::")) {
                // Out-of-line member definition: "void Foo::bar() { ... }".
                ++pos_;
                qualified_class = name;
                name = expect_identifier("a member name after '::'");
            }

            if (is_punct("(") && (first || !qualified_class.empty()) &&
                looks_like_parameter_list()) {
                Function fn;
                fn.name = name;
                fn.return_type = type;
                fn.line = decl_line;
                if (!qualified_class.empty()) fn.owner_class = qualified_class;
                else if (owner) fn.owner_class = owner->name;
                ++pos_;
                fn.params = parse_params();
                while (is_word("const") || is_word("override") || is_word("noexcept")) ++pos_;
                skip_member_init_list();
                finish_function_body(fn);
                if (owner) {
                    owner->methods.push_back(std::move(fn));
                } else if (!qualified_class.empty()) {
                    ClassDecl* target = find_class(program, qualified_class);
                    if (target) target->methods.push_back(std::move(fn));
                    else program.functions.push_back(std::move(fn));
                } else {
                    program.functions.push_back(std::move(fn));
                }
                return;
            }

            type = parse_array_suffix(type);

            if (owner) {
                Field field;
                field.name = name;
                field.type = type;
                // A member initialiser is parsed and discarded; layout owns fields.
                if (match_punct("=")) parse_assignment();
                owner->fields.push_back(std::move(field));
            } else {
                Global g;
                g.name = name;
                g.type = type;
                g.line = decl_line;
                if (match_punct("=")) {
                    g.init = parse_initialiser();
                } else if (match_punct("(")) {
                    if (!is_punct(")")) {
                        for (;;) {
                            g.ctor_args.push_back(parse_assignment());
                            if (!match_punct(",")) break;
                        }
                    }
                    expect_punct(")", "after constructor arguments");
                }
                program.globals.push_back(std::move(g));
            }

            first = false;
            if (match_punct(",")) continue;
            break;
        }
        expect_punct(";", "after a declaration");
    }

    static ClassDecl* find_class(Program& program, const std::string& name) {
        for (auto& c : program.classes)
            if (c.name == name) return &c;
        return nullptr;
    }

    // Distinguishes "int f(int x)" from "Timer t(1, 2)" once the cursor sits on '('.
    bool looks_like_parameter_list() const {
        if (is_punct(")", 1)) return true;
        if (is_word("void", 1) && is_punct(")", 2)) return true;
        const Token& t = peek(1);
        if (t.kind == Tok::Keyword && (is_base_type_word(t.text) || is_type_qualifier(t.text)))
            return true;
        if (t.kind == Tok::Identifier && (is_base_type_word(t.text) || is_type_qualifier(t.text)))
            return true;
        if (t.kind == Tok::Identifier && classes_.count(t.text)) {
            size_t k = 2;
            while (is_punct("*", k) || is_punct("&", k)) ++k;
            if (k > 2) return true;
            return peek(k).kind == Tok::Identifier;
        }
        return false;
    }

    std::vector<Param> parse_params() {
        std::vector<Param> params;
        if (match_punct(")")) return params;
        if (is_word("void") && is_punct(")", 1)) {
            pos_ += 2;
            return params;
        }
        for (;;) {
            Param p;
            p.type = parse_type();
            if (peek().kind == Tok::Identifier) p.name = advance().text;
            p.type = parse_array_suffix(p.type);
            if (match_punct("=")) parse_assignment();   // default argument: ignored
            params.push_back(std::move(p));
            if (match_punct(",")) continue;
            break;
        }
        expect_punct(")", "after a parameter list");
        return params;
    }

    // Constructor member-initialiser lists carry no information this AST models.
    void skip_member_init_list() {
        if (!is_punct(":")) return;
        ++pos_;
        while (!at_end() && !is_punct("{")) {
            if (is_punct("(")) skip_balanced("(", ")");
            else ++pos_;
        }
    }

    void skip_balanced(const std::string& open, const std::string& close) {
        int depth = 0;
        do {
            if (at_end()) error("expected '" + close + "'");
            if (is_punct(open)) ++depth;
            else if (is_punct(close)) --depth;
            ++pos_;
        } while (depth > 0);
    }

    void finish_function_body(Function& fn) {
        if (match_punct(";")) return;                       // declaration only
        if (is_punct("{")) {
            fn.body = parse_block();
            return;
        }
        error("expected '{' or ';' after a function header");
    }

    void parse_class(Program& program) {
        ++pos_;                                             // class / struct
        ClassDecl decl;
        decl.name = expect_identifier("a class name");
        classes_.insert(decl.name);
        if (is_punct(":")) {                                // base list: ignored
            while (!at_end() && !is_punct("{")) ++pos_;
        }
        expect_punct("{", "to open a class body");
        while (!is_punct("}")) {
            if (at_end()) error("expected '}' to close a class body");
            if (match_punct(";")) continue;
            if ((is_word("public") || is_word("private") || is_word("protected")) &&
                is_punct(":", 1)) {
                pos_ += 2;                                  // access specifier: ignored
                continue;
            }
            if (is_word("enum")) {
                parse_enum(program);
                continue;
            }
            parse_declaration(program, &decl);
        }
        expect_punct("}", "to close a class body");
        expect_punct(";", "after a class body");
        program.classes.push_back(std::move(decl));
    }

    // Enumerators become integer globals so the rest of the compiler can treat
    // them as ordinary named constants.
    void parse_enum(Program& program) {
        ++pos_;                                             // enum
        if (is_word("class") || is_word("struct")) ++pos_;
        if (peek().kind == Tok::Identifier && !is_punct("{")) ++pos_;
        if (is_punct(":")) {                                // fixed underlying type
            ++pos_;
            parse_type();
        }
        expect_punct("{", "to open an enum body");
        long next = 0;
        while (!is_punct("}")) {
            if (at_end()) error("expected '}' to close an enum body");
            size_t enum_line = line();
            std::string name = expect_identifier("an enumerator name");
            if (match_punct("=")) {
                ExprPtr value = parse_assignment();
                if (value->kind != ExprKind::IntLiteral)
                    error("expected a constant integer enumerator value");
                next = value->int_value;
            }
            Global g;
            g.name = name;
            g.type = make_type(TypeKind::Int);
            g.line = enum_line;
            g.init = make_int(next, enum_line);
            program.globals.push_back(std::move(g));
            ++next;
            if (!match_punct(",")) break;
        }
        expect_punct("}", "to close an enum body");
        expect_punct(";", "after an enum body");
    }

    // -------------------------------------------------------- statements ---

    StmtPtr parse_block() {
        size_t block_line = line();
        expect_punct("{", "to open a block");
        auto block = make_stmt(StmtKind::Block, block_line);
        while (!is_punct("}")) {
            if (at_end()) error("expected '}' to close a block");
            bool was_declaration = starts_declaration();
            StmtPtr stmt = parse_statement();

            // "int a = 1, b = 2;" parses into a Block of VarDecls, but a Block
            // opens a scope during semantic analysis, which would hide those
            // names from the rest of this block. Splice them in directly.
            if (was_declaration && stmt && stmt->kind == StmtKind::Block) {
                for (StmtPtr& decl : stmt->body) block->body.push_back(std::move(decl));
                continue;
            }
            block->body.push_back(std::move(stmt));
        }
        expect_punct("}", "to close a block");
        return block;
    }

    StmtPtr parse_statement() {
        size_t stmt_line = line();

        if (is_punct("{")) return parse_block();
        if (match_punct(";")) return make_stmt(StmtKind::Empty, stmt_line);

        if (is_word("if")) return parse_if();
        if (is_word("while")) return parse_while();
        if (is_word("for")) return parse_for();
        if (is_word("do")) return parse_do_while();
        if (is_word("switch")) return parse_switch();

        if (is_word("case") || is_word("default"))
            error("'" + peek().text + "' label outside of a switch statement");

        if (match_word("return")) {
            auto stmt = make_stmt(StmtKind::Return, stmt_line);
            if (!is_punct(";")) stmt->expr = parse_expression();
            expect_punct(";", "after a return statement");
            return stmt;
        }
        if (match_word("break")) {
            expect_punct(";", "after 'break'");
            return make_stmt(StmtKind::Break, stmt_line);
        }
        if (match_word("continue")) {
            expect_punct(";", "after 'continue'");
            return make_stmt(StmtKind::Continue, stmt_line);
        }
        if (is_word("class") || is_word("struct") || is_word("enum")) {
            // Local type declarations are hoisted into the enclosing program.
            if (is_word("enum")) parse_enum(program_);
            else parse_class(program_);
            return make_stmt(StmtKind::Empty, stmt_line);
        }
        if (starts_declaration()) return parse_local_declaration();

        auto stmt = make_stmt(StmtKind::Expression, stmt_line);
        stmt->expr = parse_expression();
        expect_punct(";", "after an expression statement");
        return stmt;
    }

    // A local declaration list becomes one VarDecl, or a Block of VarDecls when
    // several names share a base type.
    StmtPtr parse_local_declaration(bool want_semicolon = true) {
        size_t decl_line = line();
        TypePtr base = parse_type();
        std::vector<StmtPtr> decls;
        for (;;) {
            TypePtr type = base;
            while (match_punct("*")) type = make_pointer(type);
            auto stmt = make_stmt(StmtKind::VarDecl, decl_line);
            stmt->var_name = expect_identifier("a variable name");
            stmt->var_type = parse_array_suffix(type);
            if (match_punct("=")) {
                stmt->var_init = parse_initialiser();
            } else if (match_punct("(")) {
                if (!is_punct(")")) {
                    for (;;) {
                        stmt->ctor_args.push_back(parse_assignment());
                        if (!match_punct(",")) break;
                    }
                }
                expect_punct(")", "after constructor arguments");
            }
            decls.push_back(std::move(stmt));
            if (match_punct(",")) continue;
            break;
        }
        if (want_semicolon) expect_punct(";", "after a declaration");
        if (decls.size() == 1) return std::move(decls[0]);
        auto block = make_stmt(StmtKind::Block, decl_line);
        block->body = std::move(decls);
        return block;
    }

    // Brace initialisers are accepted for arrays and aggregates; only the first
    // element survives, which is all this AST can express.
    ExprPtr parse_initialiser() {
        if (!is_punct("{")) return parse_assignment();
        size_t brace_line = line();
        ++pos_;
        ExprPtr first;
        if (!is_punct("}")) {
            for (;;) {
                ExprPtr element = parse_initialiser();
                if (!first) first = std::move(element);
                if (!match_punct(",")) break;
                if (is_punct("}")) break;
            }
        }
        expect_punct("}", "to close a brace initialiser");
        if (!first) first = make_int(0, brace_line);
        return first;
    }

    StmtPtr parse_if() {
        size_t stmt_line = line();
        ++pos_;
        expect_punct("(", "after 'if'");
        auto stmt = make_stmt(StmtKind::If, stmt_line);
        stmt->expr = parse_expression();
        expect_punct(")", "after an if condition");
        stmt->then_branch = parse_statement();
        if (match_word("else")) stmt->else_branch = parse_statement();
        return stmt;
    }

    StmtPtr parse_while() {
        size_t stmt_line = line();
        ++pos_;
        expect_punct("(", "after 'while'");
        auto stmt = make_stmt(StmtKind::While, stmt_line);
        stmt->expr = parse_expression();
        expect_punct(")", "after a while condition");
        stmt->then_branch = parse_statement();
        return stmt;
    }

    StmtPtr parse_for() {
        size_t stmt_line = line();
        ++pos_;
        expect_punct("(", "after 'for'");
        auto stmt = make_stmt(StmtKind::For, stmt_line);
        if (!match_punct(";")) {
            if (starts_declaration()) {
                stmt->init = parse_local_declaration();
            } else {
                auto init = make_stmt(StmtKind::Expression, line());
                init->expr = parse_expression();
                expect_punct(";", "after a for initialiser");
                stmt->init = std::move(init);
            }
        }
        if (!is_punct(";")) stmt->expr = parse_expression();
        expect_punct(";", "after a for condition");
        if (!is_punct(")")) stmt->step = parse_expression();
        expect_punct(")", "after a for clause");
        stmt->then_branch = parse_statement();
        return stmt;
    }

    // "do S while (E);" is lowered to "{ S; while (E) S; }". The body is parsed
    // twice from the same tokens so both copies are independent trees.
    StmtPtr parse_do_while() {
        size_t stmt_line = line();
        ++pos_;
        size_t body_start = pos_;
        StmtPtr first_pass = parse_statement();
        size_t after_body = pos_;

        pos_ = body_start;
        StmtPtr second_pass = parse_statement();
        pos_ = after_body;

        if (!match_word("while")) error("expected 'while' after a do body");
        expect_punct("(", "after 'while'");
        auto loop = make_stmt(StmtKind::While, stmt_line);
        loop->expr = parse_expression();
        expect_punct(")", "after a while condition");
        expect_punct(";", "after a do-while statement");
        loop->then_branch = std::move(second_pass);

        auto block = make_stmt(StmtKind::Block, stmt_line);
        block->body.push_back(std::move(first_pass));
        block->body.push_back(std::move(loop));
        return block;
    }

    // True for expressions that can be re-evaluated as often as the lowering
    // likes: no calls, no assignments, no increments. A switch on one of these
    // needs no temporary at all, so the controlling expression keeps its own
    // type instead of being copied through a compiler-chosen one.
    static bool is_repeatable(const Expr& expr) {
        switch (expr.kind) {
            case ExprKind::IntLiteral:
            case ExprKind::Identifier:
                return true;
            case ExprKind::Member:
                return expr.lhs && is_repeatable(*expr.lhs);
            default:
                return false;
        }
    }

    static ExprPtr clone_repeatable(const Expr& expr, size_t l) {
        auto copy = make_expr(expr.kind, l);
        copy->int_value = expr.int_value;
        copy->name = expr.name;
        copy->through_pointer = expr.through_pointer;
        if (expr.lhs) copy->lhs = clone_repeatable(*expr.lhs, l);
        return copy;
    }

    // A run of "case"/"default" labels together with the statements that follow
    // them, up to the next label or the closing brace.
    struct SwitchGroup {
        std::vector<ExprPtr> labels;      // empty when the group is only "default"
        bool has_default = false;
        std::vector<StmtPtr> body;
        size_t line = 0;
    };

    // True when the statement contains a 'break' that belongs to the enclosing
    // switch rather than to a loop of its own. Loop bodies are not searched: a
    // 'break' inside them binds to that loop and is left alone.
    static bool has_switch_break(const Stmt& stmt) {
        switch (stmt.kind) {
            case StmtKind::Break:
                return true;
            case StmtKind::Block:
                for (const auto& child : stmt.body)
                    if (child && has_switch_break(*child)) return true;
                return false;
            case StmtKind::If:
                if (stmt.then_branch && has_switch_break(*stmt.then_branch)) return true;
                if (stmt.else_branch && has_switch_break(*stmt.else_branch)) return true;
                return false;
            default:
                return false;
        }
    }

    // Removes a trailing 'break' from a case body, reporting whether the body
    // ends by leaving the switch. A trailing 'break' becomes the end of the
    // branch, which is exactly where control lands after lowering. 'return' and
    // 'continue' already leave the switch and are kept as they are.
    static bool strip_case_terminator(std::vector<StmtPtr>& body) {
        if (body.empty()) return false;
        Stmt* last = body.back().get();
        if (!last) return false;
        if (last->kind == StmtKind::Break) {
            body.pop_back();
            return true;
        }
        if (last->kind == StmtKind::Return || last->kind == StmtKind::Continue) return true;
        if (last->kind == StmtKind::Block) return strip_case_terminator(last->body);
        return false;
    }

    // "switch (E) { case A: ... }" is lowered to a block that evaluates E once
    // into a compiler-generated local and then tests it with an if/else-if
    // chain. 'default' becomes the final else, wherever it was written.
    //
    // The AST has no label or goto node, so a case that falls through into the
    // next one cannot be expressed and is rejected rather than miscompiled.
    StmtPtr parse_switch() {
        size_t stmt_line = line();
        ++pos_;
        expect_punct("(", "after 'switch'");
        ExprPtr control = parse_expression();
        expect_punct(")", "after a switch condition");
        expect_punct("{", "to open a switch body");

        std::vector<SwitchGroup> groups;
        bool seen_default = false;
        while (!is_punct("}")) {
            if (at_end()) error("expected '}' to close a switch body");
            if (is_word("case") || is_word("default")) {
                size_t label_line = line();
                bool is_default = is_word("default");
                ++pos_;
                ExprPtr value;
                if (!is_default) value = parse_conditional();
                expect_punct(":", is_default ? "after 'default'" : "after a case label");
                // Consecutive labels with nothing between them share one group;
                // that is the only fall-through C allows without a statement.
                if (groups.empty() || !groups.back().body.empty()) {
                    SwitchGroup fresh;
                    fresh.line = label_line;
                    groups.push_back(std::move(fresh));
                }
                if (is_default) {
                    if (seen_default) error("duplicate 'default' label in a switch");
                    seen_default = true;
                    groups.back().has_default = true;
                } else {
                    groups.back().labels.push_back(std::move(value));
                }
                continue;
            }
            if (groups.empty())
                error("a statement in a switch body must follow a 'case' or 'default' label");
            groups.back().body.push_back(parse_statement());
        }
        expect_punct("}", "to close a switch body");

        for (size_t i = 0; i < groups.size(); ++i) {
            SwitchGroup& group = groups[i];
            bool terminated = strip_case_terminator(group.body);
            if (!terminated && !group.body.empty() && i + 1 < groups.size())
                throw ParseError("line " + std::to_string(group.line) +
                                 ": a 'case' that falls through into the next label is not "
                                 "supported; end it with 'break' or 'return'");
            for (const auto& stmt : group.body) {
                if (stmt && has_switch_break(*stmt))
                    throw ParseError("line " + std::to_string(stmt->line) +
                                     ": 'break' inside a switch is only supported as the last "
                                     "statement of a case");
            }
        }

        auto outer = make_stmt(StmtKind::Block, stmt_line);

        // A variable or member can simply be named again in every comparison.
        // Anything else may have side effects, so it is evaluated once into a
        // compiler-generated local and the chain tests that.
        const Expr* subject = control.get();
        std::string temp;
        if (!is_repeatable(*control)) {
            temp = "__ardio_switch" + std::to_string(switch_temps_++);
            auto decl = make_stmt(StmtKind::VarDecl, stmt_line);
            decl->var_name = temp;
            decl->var_type = make_type(TypeKind::Int);
            decl->var_init = std::move(control);
            outer->body.push_back(std::move(decl));
            subject = nullptr;
        }

        // The default group becomes the tail else; the rest chain in order.
        StmtPtr tail;
        for (auto& group : groups) {
            if (!group.has_default) continue;
            tail = make_stmt(StmtKind::Block, group.line);
            tail->body = std::move(group.body);
            break;
        }

        for (size_t i = groups.size(); i-- > 0;) {
            SwitchGroup& group = groups[i];
            if (group.has_default) continue;
            auto branch = make_stmt(StmtKind::If, group.line);
            ExprPtr cond;
            for (auto& label : group.labels) {
                ExprPtr value;
                if (subject) {
                    value = clone_repeatable(*subject, group.line);
                } else {
                    value = make_expr(ExprKind::Identifier, group.line);
                    value->name = temp;
                }
                ExprPtr test =
                    make_binary("==", std::move(value), std::move(label), group.line);
                cond = cond ? make_binary("||", std::move(cond), std::move(test), group.line)
                            : std::move(test);
            }
            branch->expr = std::move(cond);
            auto body = make_stmt(StmtKind::Block, group.line);
            body->body = std::move(group.body);
            branch->then_branch = std::move(body);
            branch->else_branch = std::move(tail);
            tail = std::move(branch);
        }

        if (tail) outer->body.push_back(std::move(tail));
        return outer;
    }

    // ------------------------------------------------------- expressions ---

    ExprPtr parse_expression() {                            // comma, lowest
        ExprPtr expr = parse_assignment();
        while (is_punct(",")) {
            size_t op_line = line();
            ++pos_;
            expr = make_binary(",", std::move(expr), parse_assignment(), op_line);
        }
        return expr;
    }

    static bool is_assign_op(const std::string& op) {
        return op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" ||
               op == "%=" || op == "&=" || op == "|=" || op == "^=" || op == "<<=" ||
               op == ">>=";
    }

    ExprPtr parse_assignment() {                            // right associative
        ExprPtr lhs = parse_conditional();
        const Token& t = peek();
        if (t.kind == Tok::Punct && is_assign_op(t.text)) {
            size_t op_line = t.line;
            std::string op = t.text;
            ++pos_;
            auto expr = make_expr(ExprKind::Assign, op_line);
            expr->op = op;
            expr->lhs = std::move(lhs);
            expr->rhs = parse_assignment();
            return expr;
        }
        return lhs;
    }

    ExprPtr parse_conditional() {
        ExprPtr cond = parse_binary(0);
        if (!is_punct("?")) return cond;
        size_t op_line = line();
        ++pos_;
        auto expr = make_expr(ExprKind::Conditional, op_line);
        expr->lhs = std::move(cond);
        expr->rhs = parse_assignment();
        expect_punct(":", "in a conditional expression");
        expr->third = parse_assignment();
        return expr;
    }

    // Binary precedence, loosest level first.
    static const std::vector<std::vector<std::string>>& levels() {
        static const std::vector<std::vector<std::string>> table = {
            {"||"},
            {"&&"},
            {"|"},
            {"^"},
            {"&"},
            {"==", "!="},
            {"<", ">", "<=", ">="},
            {"<<", ">>"},
            {"+", "-"},
            {"*", "/", "%"},
        };
        return table;
    }

    ExprPtr parse_binary(size_t level) {
        if (level >= levels().size()) return parse_unary();
        ExprPtr lhs = parse_binary(level + 1);
        for (;;) {
            const Token& t = peek();
            if (t.kind != Tok::Punct) break;
            bool matched = false;
            for (const std::string& op : levels()[level]) {
                if (t.text == op) { matched = true; break; }
            }
            if (!matched) break;
            std::string op = t.text;
            size_t op_line = t.line;
            ++pos_;
            lhs = make_binary(op, std::move(lhs), parse_binary(level + 1), op_line);
        }
        return lhs;
    }

    ExprPtr parse_unary() {
        const Token& t = peek();
        if (t.kind == Tok::Punct) {
            const std::string& op = t.text;
            if (op == "!" || op == "~" || op == "-" || op == "+" || op == "&" ||
                op == "*" || op == "++" || op == "--") {
                size_t op_line = t.line;
                ++pos_;
                auto expr = make_expr(ExprKind::Unary, op_line);
                expr->op = op;
                expr->lhs = parse_unary();
                return expr;
            }
            // A cast: '(' type ')' unary.
            if (op == "(" && starts_type(1) && is_cast()) {
                size_t op_line = t.line;
                ++pos_;
                TypePtr type = parse_type();
                expect_punct(")", "after a cast type");
                auto expr = make_expr(ExprKind::Cast, op_line);
                expr->type = type;
                expr->lhs = parse_unary();
                return expr;
            }
        }
        return parse_postfix();
    }

    // Confirms that '(' at the cursor introduces a cast and not a parenthesised
    // expression such as "(count) + 1".
    bool is_cast() const {
        size_t k = 1;
        bool saw_builtin = false;
        for (;;) {
            const Token& t = peek(k);
            if ((t.kind == Tok::Keyword || t.kind == Tok::Identifier) &&
                (is_base_type_word(t.text) || is_type_qualifier(t.text))) {
                saw_builtin = true;
                ++k;
                continue;
            }
            if (!saw_builtin && t.kind == Tok::Identifier && classes_.count(t.text)) {
                ++k;
                // A class-typed cast must be to a pointer: "(Foo*)p".
                if (!is_punct("*", k)) return false;
                break;
            }
            break;
        }
        if (!saw_builtin && k == 1) return false;
        while (is_punct("*", k)) ++k;
        return is_punct(")", k);
    }

    ExprPtr parse_postfix() {
        ExprPtr expr = parse_primary();
        for (;;) {
            size_t op_line = line();
            if (match_punct("(")) {
                auto call = make_expr(ExprKind::Call, op_line);
                if (expr->kind == ExprKind::Identifier || expr->kind == ExprKind::Member)
                    call->name = expr->name;
                // A plain named call carries its callee in `name` only. Keeping
                // the identifier as `lhs` too would make semantic analysis look
                // the function up as if it were a variable. Member calls do keep
                // their object expression.
                if (expr->kind == ExprKind::Identifier)
                    expr.reset();
                call->lhs = std::move(expr);
                if (!is_punct(")")) {
                    for (;;) {
                        call->args.push_back(parse_assignment());
                        if (!match_punct(",")) break;
                    }
                }
                expect_punct(")", "after call arguments");
                expr = std::move(call);
            } else if (match_punct("[")) {
                auto index = make_expr(ExprKind::Index, op_line);
                index->lhs = std::move(expr);
                index->rhs = parse_expression();
                expect_punct("]", "after a subscript");
                expr = std::move(index);
            } else if (is_punct(".") || is_punct("->")) {
                bool arrow = is_punct("->");
                ++pos_;
                auto member = make_expr(ExprKind::Member, op_line);
                member->through_pointer = arrow;
                member->lhs = std::move(expr);
                member->name = expect_identifier("a member name");
                expr = std::move(member);
            } else if (is_punct("++") || is_punct("--")) {
                auto unary = make_expr(ExprKind::Unary, op_line);
                unary->op = advance().text;
                unary->is_postfix = true;
                unary->lhs = std::move(expr);
                expr = std::move(unary);
            } else {
                break;
            }
        }
        return expr;
    }

    ExprPtr parse_primary() {
        const Token& t = peek();
        switch (t.kind) {
            case Tok::Number:
            case Tok::CharLit: {
                ++pos_;
                return make_int(t.value, t.line);
            }
            case Tok::StringLit: {
                ++pos_;
                auto expr = make_expr(ExprKind::StringLiteral, t.line);
                expr->str_value = t.text;
                return expr;
            }
            case Tok::Identifier: {
                ++pos_;
                auto expr = make_expr(ExprKind::Identifier, t.line);
                expr->name = t.text;
                while (is_punct("::")) {                    // qualified name: flattened
                    ++pos_;
                    expr->name = expect_identifier("a name after '::'");
                }
                return expr;
            }
            case Tok::Keyword: {
                if (t.text == "true" || t.text == "false") {
                    ++pos_;
                    return make_int(t.text == "true" ? 1 : 0, t.line);
                }
                if (t.text == "nullptr" || t.text == "NULL") {
                    ++pos_;
                    return make_int(0, t.line);
                }
                if (t.text == "this") {
                    ++pos_;
                    auto expr = make_expr(ExprKind::Identifier, t.line);
                    expr->name = "this";
                    return expr;
                }
                break;
            }
            case Tok::Punct: {
                if (t.text == "(") {
                    ++pos_;
                    ExprPtr inner = parse_expression();
                    expect_punct(")", "after a parenthesised expression");
                    return inner;
                }
                break;
            }
            default:
                break;
        }
        error("expected an expression");
    }

    // ----------------------------------------------------------- helpers ---

    static ExprPtr make_expr(ExprKind kind, size_t l) {
        auto expr = std::make_unique<Expr>();
        expr->kind = kind;
        expr->line = l;
        return expr;
    }

    static ExprPtr make_int(long value, size_t l) {
        auto expr = make_expr(ExprKind::IntLiteral, l);
        expr->int_value = value;
        return expr;
    }

    static ExprPtr make_binary(const std::string& op, ExprPtr lhs, ExprPtr rhs, size_t l) {
        auto expr = make_expr(ExprKind::Binary, l);
        expr->op = op;
        expr->lhs = std::move(lhs);
        expr->rhs = std::move(rhs);
        return expr;
    }

    static StmtPtr make_stmt(StmtKind kind, size_t l) {
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = kind;
        stmt->line = l;
        return stmt;
    }

    const std::vector<Token>& toks_;
    Program& program_;
    size_t pos_ = 0;
    size_t switch_temps_ = 0;
    std::set<std::string> classes_;
};

} // namespace

ParseResult parse(const std::vector<Token>& tokens) {
    ParseResult result;
    Parser parser(tokens, result.program);
    try {
        parser.parse_program();
        result.ok = true;
    } catch (const ParseError& e) {
        result.ok = false;
        result.error = e.what();
        result.program = Program{};
    }
    return result;
}

} // namespace ardio
