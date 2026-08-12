#pragma once
#include <memory>
#include <string>
#include <vector>

// The shared AST for ardio's AVR compiler. Every front-end and back-end module
// agrees on these nodes; nothing invents its own tree.
//
// The tree is deliberately plain: owning unique_ptr children, no visitors, no
// virtual dispatch beyond the node kind tag. Code generation switches on kind.

namespace ardio {

// --------------------------------------------------------------- types -----

enum class TypeKind { Void, Bool, Char, Int, UInt, Long, ULong, Pointer, Array, Class };

struct Type;
using TypePtr = std::shared_ptr<Type>;

struct Type {
    TypeKind kind = TypeKind::Int;
    bool is_signed = true;
    TypePtr pointee;            // Pointer / Array element
    long array_length = 0;      // Array only
    std::string class_name;     // Class only

    // Size in bytes on AVR: char/bool 1, int 2, long 4, pointer 2.
    int size() const;
};

TypePtr make_type(TypeKind k);
TypePtr make_pointer(TypePtr to);
TypePtr make_array(TypePtr elem, long n);

// --------------------------------------------------------- expressions -----

enum class ExprKind {
    IntLiteral, StringLiteral, Identifier, Unary, Binary, Assign,
    Call, Index, Member, Cast, Conditional,
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    ExprKind kind = ExprKind::IntLiteral;
    size_t line = 0;
    TypePtr type;               // filled in by semantic analysis

    long int_value = 0;         // IntLiteral
    std::string str_value;      // StringLiteral
    std::string name;           // Identifier, Member (member name), Call (callee name)
    std::string op;             // Unary / Binary / Assign operator spelling

    ExprPtr lhs;                // Unary operand, Binary/Assign left, Index base,
                                // Member object, Cast operand, Conditional cond
    ExprPtr rhs;                // Binary/Assign right, Index subscript, Conditional then
    ExprPtr third;              // Conditional else
    std::vector<ExprPtr> args;  // Call arguments
    bool is_postfix = false;    // for ++ / --
    bool through_pointer = false; // Member: true for '->', false for '.'
};

// ---------------------------------------------------------- statements -----

enum class StmtKind {
    Expression, VarDecl, Block, If, While, For, Return, Break, Continue, Empty,
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Stmt {
    StmtKind kind = StmtKind::Empty;
    size_t line = 0;

    ExprPtr expr;                    // Expression, Return value, If/While condition
    std::vector<StmtPtr> body;       // Block contents
    StmtPtr then_branch;             // If / While / For body
    StmtPtr else_branch;             // If
    StmtPtr init;                    // For initialiser
    ExprPtr step;                    // For increment

    std::string var_name;            // VarDecl
    TypePtr var_type;                // VarDecl
    ExprPtr var_init;                // VarDecl initialiser
    std::vector<ExprPtr> ctor_args;  // VarDecl with constructor arguments
};

// ---------------------------------------------------------- top level  -----

struct Param {
    std::string name;
    TypePtr type;
};

struct Function {
    std::string name;
    TypePtr return_type;
    std::vector<Param> params;
    StmtPtr body;                    // null for a declaration with no definition
    std::string owner_class;         // empty for free functions
    bool is_constructor = false;
    size_t line = 0;
};

struct Field {
    std::string name;
    TypePtr type;
    int offset = 0;                  // byte offset, filled in by layout
};

struct ClassDecl {
    std::string name;
    std::vector<Field> fields;
    std::vector<Function> methods;
    int size = 0;                    // total bytes, filled in by layout
};

struct Global {
    std::string name;
    TypePtr type;
    ExprPtr init;
    std::vector<ExprPtr> ctor_args;
    size_t line = 0;
};

struct Program {
    std::vector<Global> globals;
    std::vector<Function> functions;
    std::vector<ClassDecl> classes;
};

} // namespace ardio
