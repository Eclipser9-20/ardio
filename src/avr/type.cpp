// Type sizing and constructors for ardio's AVR compiler.
//
// The ATmega328P is an 8-bit machine with a 16-bit address space, so the
// layout here is the avr-gcc one: int is 2 bytes, long is 4, and every
// pointer -- data or function -- is 2. There is no alignment requirement on
// AVR, so aggregate layout is plain byte packing (see sema.cpp).

#include "ardio/avr/ast.h"
#include "ardio/avr/sema.h"

#include <map>

namespace ardio {
namespace {

// Class sizes are computed once by the layout pass in sema.cpp; Type only
// carries the class name, so it looks the answer up here.
std::map<std::string, int>& class_sizes() {
    static std::map<std::string, int> sizes;
    return sizes;
}

} // namespace

void set_class_size(const std::string& name, int size) {
    class_sizes()[name] = size;
}

int lookup_class_size(const std::string& name) {
    auto it = class_sizes().find(name);
    return it == class_sizes().end() ? 0 : it->second;
}

void clear_class_sizes() {
    class_sizes().clear();
}

int Type::size() const {
    switch (kind) {
    case TypeKind::Void:    return 0;
    case TypeKind::Bool:    return 1;
    case TypeKind::Char:    return 1;
    case TypeKind::Int:     return 2;
    case TypeKind::UInt:    return 2;
    case TypeKind::Long:    return 4;
    case TypeKind::ULong:   return 4;
    case TypeKind::Pointer: return 2;
    case TypeKind::Array:   return pointee ? static_cast<int>(pointee->size() * array_length) : 0;
    case TypeKind::Class:   return lookup_class_size(class_name);
    }
    return 0;
}

TypePtr make_type(TypeKind k) {
    auto t = std::make_shared<Type>();
    t->kind = k;
    t->is_signed = (k == TypeKind::Char || k == TypeKind::Int || k == TypeKind::Long);
    return t;
}

TypePtr make_pointer(TypePtr to) {
    auto t = make_type(TypeKind::Pointer);
    t->is_signed = false;
    t->pointee = std::move(to);
    return t;
}

TypePtr make_array(TypePtr elem, long n) {
    auto t = make_type(TypeKind::Array);
    t->is_signed = false;
    t->pointee = std::move(elem);
    t->array_length = n;
    return t;
}

} // namespace ardio
