// Semantic analysis: class layout, type assignment, and the handful of checks
// that catch the mistakes a code generator cannot recover from.
//
// The pass is deliberately single-threaded through one Sema object holding a
// scope stack. Errors are thrown as an internal exception and caught at the
// top so that the first problem wins and nothing downstream sees a
// half-typed tree.

#include "ardio/avr/sema.h"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace ardio {
namespace {

struct SemaError {
    std::string message;
};

[[noreturn]] void fail(size_t line, const std::string& what) {
    throw SemaError{"line " + std::to_string(line) + ": " + what};
}

bool is_integer(TypeKind k) {
    switch (k) {
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::Int:
    case TypeKind::UInt:
    case TypeKind::Long:
    case TypeKind::ULong:
        return true;
    default:
        return false;
    }
}

bool is_pointerish(const TypePtr& t) {
    return t && (t->kind == TypeKind::Pointer || t->kind == TypeKind::Array);
}

// Scalars are the things a condition or a jump can be built from.
bool is_scalar(const TypePtr& t) {
    return t && (is_integer(t->kind) || is_pointerish(t));
}

std::string describe(const TypePtr& t) {
    if (!t) return "<untyped>";
    switch (t->kind) {
    case TypeKind::Void:    return "void";
    case TypeKind::Bool:    return "bool";
    case TypeKind::Char:    return "char";
    case TypeKind::Int:     return "int";
    case TypeKind::UInt:    return "unsigned int";
    case TypeKind::Long:    return "long";
    case TypeKind::ULong:   return "unsigned long";
    case TypeKind::Pointer: return describe(t->pointee) + "*";
    case TypeKind::Array:   return describe(t->pointee) + "[" + std::to_string(t->array_length) + "]";
    case TypeKind::Class:   return t->class_name;
    }
    return "?";
}

// An array used as a value becomes a pointer to its first element. The
// declared type on the declaration itself is left alone so that layout and
// storage allocation still see the array.
TypePtr decay(const TypePtr& t) {
    if (t && t->kind == TypeKind::Array) return make_pointer(t->pointee);
    return t;
}

// Integer conversion rank, used to pick the wider of two operand types.
int rank(TypeKind k) {
    switch (k) {
    case TypeKind::Bool:  return 0;
    case TypeKind::Char:  return 1;
    case TypeKind::Int:
    case TypeKind::UInt:  return 2;
    case TypeKind::Long:
    case TypeKind::ULong: return 3;
    default:              return -1;
    }
}

// Integer promotion: anything narrower than int arrives as int, because the
// AVR ALU works on registers and the compiler never keeps a sub-int
// intermediate value.
TypePtr promote(const TypePtr& t) {
    if (!t) return t;
    if (t->kind == TypeKind::Bool || t->kind == TypeKind::Char) return make_type(TypeKind::Int);
    return t;
}

// The usual arithmetic conversions, restricted to the integer types ardio
// supports: promote both sides, take the higher rank, and let unsigned win a
// tie.
TypePtr usual_conversions(const TypePtr& a, const TypePtr& b) {
    TypePtr l = promote(a);
    TypePtr r = promote(b);
    if (rank(l->kind) > rank(r->kind)) return l;
    if (rank(r->kind) > rank(l->kind)) return r;
    if (!l->is_signed) return l;
    if (!r->is_signed) return r;
    return l;
}

bool is_lvalue(const Expr& e) {
    switch (e.kind) {
    case ExprKind::Identifier:
    case ExprKind::Index:
    case ExprKind::Member:
        return true;
    case ExprKind::Unary:
        return e.op == "*";
    default:
        return false;
    }
}

bool same_pointee(const TypePtr& a, const TypePtr& b) {
    const TypePtr& x = a->pointee;
    const TypePtr& y = b->pointee;
    if (!x || !y) return true;                      // void* talks to anything
    if (x->kind == TypeKind::Void || y->kind == TypeKind::Void) return true;
    if (x->kind != y->kind) return false;
    if (x->kind == TypeKind::Class) return x->class_name == y->class_name;
    return true;
}

// Assignment compatibility. Integers convert freely in both directions --
// narrowing an int into a char is the normal way embedded code writes a port
// register, so it is allowed silently.
bool assignable(const TypePtr& target, const TypePtr& value) {
    if (!target || !value) return false;
    TypePtr v = decay(value);
    if (is_integer(target->kind) && is_integer(v->kind)) return true;
    if (target->kind == TypeKind::Pointer) {
        if (v->kind == TypeKind::Pointer) return same_pointee(target, v);
        // A null pointer constant is just an integer zero at this level.
        return is_integer(v->kind);
    }
    if (target->kind == TypeKind::Bool && is_pointerish(v)) return true;
    if (target->kind == TypeKind::Class && v->kind == TypeKind::Class)
        return target->class_name == v->class_name;
    if (target->kind == TypeKind::Array && v->kind == TypeKind::Array)
        return target->array_length == v->array_length;
    return false;
}

bool is_comparison(const std::string& op) {
    return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
}

bool is_logical(const std::string& op) {
    return op == "&&" || op == "||";
}

bool is_shift(const std::string& op) {
    return op == "<<" || op == ">>";
}

bool is_bitwise(const std::string& op) {
    return op == "&" || op == "|" || op == "^";
}

struct FuncSig {
    TypePtr return_type;
    size_t param_count = 0;
    const Function* decl = nullptr;
};

class Sema {
public:
    explicit Sema(Program& program) : program_(program) {}

    void run() {
        layout_classes();
        collect_functions();

        scopes_.emplace_back();
        for (auto& g : program_.globals) declare_global(g);

        for (auto& f : program_.functions) analyse_function(f, nullptr);
        for (auto& c : program_.classes)
            for (auto& m : c.methods) analyse_function(m, &c);

        scopes_.pop_back();
    }

private:
    // ------------------------------------------------------------ layout ---

    void layout_classes() {
        clear_class_sizes();
        for (auto& c : program_.classes) classes_[c.name] = &c;
        // Two passes so that a class may contain a value member of a class
        // declared later in the file; sizes are resolved by name at use.
        for (auto& c : program_.classes) layout_one(c);
    }

    void layout_one(ClassDecl& c) {
        int offset = 0;
        for (auto& f : c.fields) {
            if (!f.type) fail(0, "field '" + f.name + "' of class '" + c.name + "' has no type");
            if (f.type->kind == TypeKind::Class && f.type->class_name != c.name) {
                auto it = classes_.find(f.type->class_name);
                if (it == classes_.end())
                    fail(0, "unknown class '" + f.type->class_name + "' used as a field");
                if (lookup_class_size(it->second->name) == 0) layout_one(*it->second);
            }
            f.offset = offset;
            offset += f.type->size();
        }
        c.size = offset;
        set_class_size(c.name, offset);
    }

    // --------------------------------------------------------- functions ---

    void collect_functions() {
        for (auto& f : program_.functions) {
            if (!f.return_type) f.return_type = make_type(TypeKind::Void);
            functions_[f.name] = FuncSig{f.return_type, f.params.size(), &f};
        }
        for (auto& c : program_.classes)
            for (auto& m : c.methods) {
                if (!m.return_type) m.return_type = make_type(TypeKind::Void);
                methods_[c.name + "::" + m.name] = FuncSig{m.return_type, m.params.size(), &m};
            }
    }

    void declare_global(Global& g) {
        if (!g.type) fail(g.line, "global '" + g.name + "' has no type");
        declare(g.name, g.type, g.line);
        if (g.init) {
            TypePtr t = check(*g.init);
            if (!assignable(g.type, t))
                fail(g.line, "cannot initialise " + describe(g.type) + " from " + describe(t));
        }
        check_ctor_args(g.type, g.ctor_args, g.line);
    }

    void analyse_function(Function& f, const ClassDecl* owner) {
        if (!f.body) return;
        current_return_ = f.return_type ? f.return_type : make_type(TypeKind::Void);
        current_class_ = owner;
        scopes_.emplace_back();
        if (owner) {
            auto self = make_type(TypeKind::Class);
            self->class_name = owner->name;
            declare("this", make_pointer(self), f.line);
        }
        for (auto& p : f.params) {
            if (!p.type) fail(f.line, "parameter '" + p.name + "' has no type");
            declare(p.name, p.type, f.line);
        }
        check_stmt(*f.body);
        scopes_.pop_back();
        current_class_ = nullptr;
    }

    // ------------------------------------------------------------ scopes ---

    void declare(const std::string& name, TypePtr type, size_t line) {
        if (name.empty()) return;
        auto& top = scopes_.back();
        if (top.count(name)) fail(line, "redeclaration of '" + name + "'");
        top[name] = std::move(type);
    }

    TypePtr lookup(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return found->second;
        }
        // Inside a method, an unqualified name may be a field of the class.
        if (current_class_)
            for (const auto& f : current_class_->fields)
                if (f.name == name) return f.type;
        return nullptr;
    }

    // -------------------------------------------------------- statements ---

    void check_stmt(Stmt& s) {
        switch (s.kind) {
        case StmtKind::Empty:
        case StmtKind::Break:
        case StmtKind::Continue:
            break;

        case StmtKind::Expression:
            if (s.expr) check(*s.expr);
            break;

        case StmtKind::VarDecl: {
            if (!s.var_type) fail(s.line, "variable '" + s.var_name + "' has no type");
            if (s.var_type->kind == TypeKind::Class &&
                !classes_.count(s.var_type->class_name))
                fail(s.line, "unknown class '" + s.var_type->class_name + "'");
            if (s.var_init) {
                TypePtr t = check(*s.var_init);
                if (!assignable(s.var_type, t))
                    fail(s.line, "cannot initialise " + describe(s.var_type) + " from " + describe(t));
            }
            check_ctor_args(s.var_type, s.ctor_args, s.line);
            declare(s.var_name, s.var_type, s.line);
            break;
        }

        case StmtKind::Block:
            scopes_.emplace_back();
            for (auto& child : s.body)
                if (child) check_stmt(*child);
            scopes_.pop_back();
            break;

        case StmtKind::If:
            require_condition(s);
            if (s.then_branch) check_stmt(*s.then_branch);
            if (s.else_branch) check_stmt(*s.else_branch);
            break;

        case StmtKind::While:
            require_condition(s);
            if (s.then_branch) check_stmt(*s.then_branch);
            break;

        case StmtKind::For:
            scopes_.emplace_back();
            if (s.init) check_stmt(*s.init);
            if (s.expr) require_condition(s);
            if (s.step) check(*s.step);
            if (s.then_branch) check_stmt(*s.then_branch);
            scopes_.pop_back();
            break;

        case StmtKind::Return: {
            TypePtr want = current_return_ ? current_return_ : make_type(TypeKind::Void);
            if (!s.expr) {
                if (want->kind != TypeKind::Void)
                    fail(s.line, "return with no value in a function returning " + describe(want));
                break;
            }
            TypePtr got = check(*s.expr);
            if (want->kind == TypeKind::Void)
                fail(s.line, "return with a value in a function returning void");
            if (!assignable(want, got))
                fail(s.line, "cannot return " + describe(got) + " from a function returning " +
                                 describe(want));
            break;
        }
        }
    }

    void require_condition(Stmt& s) {
        if (!s.expr) return;
        TypePtr t = decay(check(*s.expr));
        if (!is_scalar(t)) fail(s.line, describe(t) + " is not usable as a condition");
    }

    void check_ctor_args(const TypePtr& type, std::vector<ExprPtr>& args, size_t line) {
        if (args.empty()) return;
        for (auto& a : args)
            if (a) check(*a);
        if (!type || type->kind != TypeKind::Class)
            fail(line, "constructor arguments given for non-class type " + describe(type));
        auto cls = classes_.find(type->class_name);
        if (cls == classes_.end()) fail(line, "unknown class '" + type->class_name + "'");
        for (const auto& m : cls->second->methods) {
            if (!m.is_constructor) continue;
            if (m.params.size() != args.size())
                fail(line, "constructor of '" + type->class_name + "' expects " +
                               std::to_string(m.params.size()) + " arguments, got " +
                               std::to_string(args.size()));
            return;
        }
    }

    // ------------------------------------------------------- expressions ---

    TypePtr check(Expr& e) {
        switch (e.kind) {
        case ExprKind::IntLiteral:
            e.type = (e.int_value > 32767 || e.int_value < -32768) ? make_type(TypeKind::Long)
                                                                   : make_type(TypeKind::Int);
            break;

        case ExprKind::StringLiteral:
            e.type = make_array(make_type(TypeKind::Char),
                                static_cast<long>(e.str_value.size()) + 1);
            break;

        case ExprKind::Identifier: {
            TypePtr t = lookup(e.name);
            if (!t) fail(e.line, "undeclared identifier '" + e.name + "'");
            e.type = t;
            break;
        }

        case ExprKind::Unary:       e.type = check_unary(e); break;
        case ExprKind::Binary:      e.type = check_binary(e); break;
        case ExprKind::Assign:      e.type = check_assign(e); break;
        case ExprKind::Call:        e.type = check_call(e); break;
        case ExprKind::Index:       e.type = check_index(e); break;
        case ExprKind::Member:      e.type = check_member(e); break;
        case ExprKind::Conditional: e.type = check_conditional(e); break;

        case ExprKind::Cast:
            if (!e.lhs) fail(e.line, "cast without an operand");
            check(*e.lhs);
            if (!e.type) fail(e.line, "cast without a target type");
            break;
        }
        return e.type;
    }

    TypePtr check_unary(Expr& e) {
        if (!e.lhs) fail(e.line, "unary '" + e.op + "' without an operand");
        TypePtr t = decay(check(*e.lhs));

        if (e.op == "!") {
            if (!is_scalar(t)) fail(e.line, "'!' needs a scalar operand, got " + describe(t));
            return make_type(TypeKind::Bool);
        }
        if (e.op == "&") {
            if (!is_lvalue(*e.lhs)) fail(e.line, "cannot take the address of a non-lvalue");
            return make_pointer(e.lhs->type);
        }
        if (e.op == "*") {
            if (!is_pointerish(t)) fail(e.line, "cannot dereference " + describe(t));
            if (!t->pointee || t->pointee->kind == TypeKind::Void)
                fail(e.line, "cannot dereference a void pointer");
            return t->pointee;
        }
        if (e.op == "++" || e.op == "--") {
            if (!is_lvalue(*e.lhs)) fail(e.line, "'" + e.op + "' needs an lvalue operand");
            if (!is_scalar(t)) fail(e.line, "'" + e.op + "' needs a scalar operand, got " + describe(t));
            return e.lhs->type;
        }
        if (e.op == "-" || e.op == "+" || e.op == "~") {
            if (!t || !is_integer(t->kind))
                fail(e.line, "unary '" + e.op + "' needs an integer operand, got " + describe(t));
            return promote(t);
        }
        fail(e.line, "unknown unary operator '" + e.op + "'");
    }

    TypePtr check_binary(Expr& e) {
        if (!e.lhs || !e.rhs) fail(e.line, "binary '" + e.op + "' is missing an operand");
        TypePtr l = decay(check(*e.lhs));
        TypePtr r = decay(check(*e.rhs));

        if (is_logical(e.op)) {
            if (!is_scalar(l) || !is_scalar(r))
                fail(e.line, "'" + e.op + "' needs scalar operands");
            return make_type(TypeKind::Bool);
        }

        if (is_comparison(e.op)) {
            bool lp = is_pointerish(l), rp = is_pointerish(r);
            if (lp != rp && !(is_integer(lp ? r->kind : l->kind)))
                fail(e.line, "cannot compare " + describe(l) + " with " + describe(r));
            if (!lp && !rp && (!is_scalar(l) || !is_scalar(r)))
                fail(e.line, "cannot compare " + describe(l) + " with " + describe(r));
            return make_type(TypeKind::Bool);
        }

        if (is_shift(e.op)) {
            if (!l || !r || !is_integer(l->kind) || !is_integer(r->kind))
                fail(e.line, "'" + e.op + "' needs integer operands");
            // Only the left operand's type survives a shift.
            return promote(l);
        }

        if (is_bitwise(e.op) || e.op == "%") {
            if (!l || !r || !is_integer(l->kind) || !is_integer(r->kind))
                fail(e.line, "'" + e.op + "' needs integer operands");
            return usual_conversions(l, r);
        }

        if (e.op == "+" || e.op == "-") {
            // Pointer arithmetic: the offset is scaled by the pointee size in
            // the back end; here it only has to produce the right type.
            if (is_pointerish(l) && is_integer(r->kind)) return decay(l);
            if (e.op == "+" && is_integer(l->kind) && is_pointerish(r)) return decay(r);
            if (e.op == "-" && is_pointerish(l) && is_pointerish(r)) {
                if (!same_pointee(l, r))
                    fail(e.line, "cannot subtract " + describe(r) + " from " + describe(l));
                return make_type(TypeKind::Int);   // ptrdiff_t on AVR is 16-bit
            }
            if (is_pointerish(l) || is_pointerish(r))
                fail(e.line, "invalid pointer arithmetic between " + describe(l) + " and " +
                                 describe(r));
        }

        if (e.op == "*" || e.op == "/" || e.op == "+" || e.op == "-") {
            if (!l || !r || !is_integer(l->kind) || !is_integer(r->kind))
                fail(e.line, "'" + e.op + "' needs arithmetic operands, got " + describe(l) +
                                 " and " + describe(r));
            return usual_conversions(l, r);
        }

        fail(e.line, "unknown binary operator '" + e.op + "'");
    }

    TypePtr check_assign(Expr& e) {
        if (!e.lhs || !e.rhs) fail(e.line, "assignment is missing an operand");
        TypePtr target = check(*e.lhs);
        TypePtr value = check(*e.rhs);
        if (!is_lvalue(*e.lhs)) fail(e.line, "assignment to a non-lvalue");
        if (target && target->kind == TypeKind::Array)
            fail(e.line, "assignment to an array");

        if (e.op != "=" && !e.op.empty()) {
            // Compound assignment: pointer += int keeps the pointer type,
            // everything else must be arithmetic.
            std::string base = e.op.substr(0, e.op.size() - 1);
            if (is_pointerish(target) && (base == "+" || base == "-")) {
                if (!value || !is_integer(value->kind))
                    fail(e.line, "'" + e.op + "' on a pointer needs an integer operand");
                return target;
            }
            if (!target || !is_integer(target->kind) || !value || !is_integer(decay(value)->kind))
                fail(e.line, "'" + e.op + "' needs arithmetic operands");
            return target;
        }

        if (!assignable(target, value))
            fail(e.line, "cannot assign " + describe(value) + " to " + describe(target));
        return target;
    }

    TypePtr check_call(Expr& e) {
        for (auto& a : e.args) {
            if (!a) fail(e.line, "missing argument in call to '" + e.name + "'");
            check(*a);
        }

        const FuncSig* sig = nullptr;
        std::string shown = e.name;

        if (e.lhs) {
            // Method call: the callee object was parsed as the left child.
            TypePtr obj = check(*e.lhs);
            if (is_pointerish(obj)) obj = obj->pointee;
            if (!obj || obj->kind != TypeKind::Class)
                fail(e.line, "cannot call a method on " + describe(obj));
            shown = obj->class_name + "::" + e.name;
            auto it = methods_.find(shown);
            if (it == methods_.end()) fail(e.line, "undeclared method '" + shown + "'");
            sig = &it->second;
        } else {
            auto it = functions_.find(e.name);
            if (it == functions_.end()) {
                if (current_class_) {
                    auto m = methods_.find(current_class_->name + "::" + e.name);
                    if (m != methods_.end()) sig = &m->second;
                }
                if (!sig) fail(e.line, "undeclared function '" + e.name + "'");
            } else {
                sig = &it->second;
            }
        }

        if (sig->param_count != e.args.size())
            fail(e.line, "'" + shown + "' expects " + std::to_string(sig->param_count) +
                             " arguments, got " + std::to_string(e.args.size()));

        for (size_t i = 0; i < e.args.size(); ++i) {
            const TypePtr& want = sig->decl->params[i].type;
            if (want && !assignable(want, e.args[i]->type))
                fail(e.line, "argument " + std::to_string(i + 1) + " of '" + shown +
                                 "' expects " + describe(want) + ", got " +
                                 describe(e.args[i]->type));
        }
        return sig->return_type ? sig->return_type : make_type(TypeKind::Void);
    }

    TypePtr check_index(Expr& e) {
        if (!e.lhs || !e.rhs) fail(e.line, "subscript is missing an operand");
        TypePtr base = decay(check(*e.lhs));
        TypePtr sub = decay(check(*e.rhs));
        if (!is_pointerish(base)) fail(e.line, "cannot subscript " + describe(base));
        if (!sub || !is_integer(sub->kind))
            fail(e.line, "array subscript must be an integer, got " + describe(sub));
        if (!base->pointee) fail(e.line, "cannot subscript a pointer to an incomplete type");
        return base->pointee;
    }

    TypePtr check_member(Expr& e) {
        if (!e.lhs) fail(e.line, "member access without an object");
        TypePtr obj = check(*e.lhs);
        if (e.through_pointer) {
            if (!is_pointerish(obj)) fail(e.line, "'->' applied to " + describe(obj));
            obj = obj->pointee;
        } else if (obj && obj->kind == TypeKind::Array) {
            fail(e.line, "'.' applied to " + describe(obj));
        }
        if (!obj || obj->kind != TypeKind::Class)
            fail(e.line, "member access on non-class type " + describe(obj));
        auto cls = classes_.find(obj->class_name);
        if (cls == classes_.end()) fail(e.line, "unknown class '" + obj->class_name + "'");
        for (const auto& f : cls->second->fields)
            if (f.name == e.name) return f.type;
        for (const auto& m : cls->second->methods)
            if (m.name == e.name) return m.return_type;
        fail(e.line, "class '" + obj->class_name + "' has no member '" + e.name + "'");
    }

    TypePtr check_conditional(Expr& e) {
        if (!e.lhs || !e.rhs || !e.third) fail(e.line, "conditional is missing an operand");
        TypePtr cond = decay(check(*e.lhs));
        if (!is_scalar(cond)) fail(e.line, describe(cond) + " is not usable as a condition");
        TypePtr a = decay(check(*e.rhs));
        TypePtr b = decay(check(*e.third));
        if (a && b && is_integer(a->kind) && is_integer(b->kind)) return usual_conversions(a, b);
        if (is_pointerish(a) && is_pointerish(b) && same_pointee(a, b)) return a;
        if (a && b && a->kind == b->kind && a->kind == TypeKind::Class &&
            a->class_name == b->class_name)
            return a;
        if (a && b && a->kind == TypeKind::Void && b->kind == TypeKind::Void)
            return a;
        fail(e.line, "conditional branches have incompatible types " + describe(a) + " and " +
                         describe(b));
    }

    Program& program_;
    std::vector<std::map<std::string, TypePtr>> scopes_;
    std::map<std::string, ClassDecl*> classes_;
    std::map<std::string, FuncSig> functions_;
    std::map<std::string, FuncSig> methods_;
    TypePtr current_return_;
    const ClassDecl* current_class_ = nullptr;
};

} // namespace

SemaResult analyse(Program& program) {
    SemaResult result;
    try {
        Sema sema(program);
        sema.run();
        result.ok = true;
    } catch (const SemaError& e) {
        result.ok = false;
        result.error = e.message;
    }
    return result;
}

} // namespace ardio
