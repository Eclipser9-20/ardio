#include "harness.h"

#include "ardio/avr/ast.h"
#include "ardio/avr/sema.h"

using namespace ardio;

TEST(type_scalar_sizes_match_avr) {
    CHECK_EQ(make_type(TypeKind::Void)->size(), 0);
    CHECK_EQ(make_type(TypeKind::Bool)->size(), 1);
    CHECK_EQ(make_type(TypeKind::Char)->size(), 1);
    CHECK_EQ(make_type(TypeKind::Int)->size(), 2);
    CHECK_EQ(make_type(TypeKind::UInt)->size(), 2);
    CHECK_EQ(make_type(TypeKind::Long)->size(), 4);
    CHECK_EQ(make_type(TypeKind::ULong)->size(), 4);
}

TEST(type_signedness_defaults) {
    CHECK(make_type(TypeKind::Int)->is_signed);
    CHECK(make_type(TypeKind::Char)->is_signed);
    CHECK(make_type(TypeKind::Long)->is_signed);
    CHECK(!make_type(TypeKind::UInt)->is_signed);
    CHECK(!make_type(TypeKind::ULong)->is_signed);
}

TEST(type_pointers_are_two_bytes) {
    CHECK_EQ(make_pointer(make_type(TypeKind::Char))->size(), 2);
    CHECK_EQ(make_pointer(make_type(TypeKind::Long))->size(), 2);
    // A pointer to a pointer is still just an address.
    CHECK_EQ(make_pointer(make_pointer(make_type(TypeKind::Int)))->size(), 2);
}

TEST(type_pointer_records_its_pointee) {
    auto p = make_pointer(make_type(TypeKind::Long));
    CHECK(p->kind == TypeKind::Pointer);
    CHECK(p->pointee->kind == TypeKind::Long);
}

TEST(type_array_size_is_element_times_length) {
    CHECK_EQ(make_array(make_type(TypeKind::Char), 10)->size(), 10);
    CHECK_EQ(make_array(make_type(TypeKind::Int), 10)->size(), 20);
    CHECK_EQ(make_array(make_type(TypeKind::Long), 3)->size(), 12);
    CHECK_EQ(make_array(make_type(TypeKind::Int), 0)->size(), 0);
}

TEST(type_array_of_arrays_multiplies) {
    auto inner = make_array(make_type(TypeKind::Int), 4);   // 8 bytes
    auto outer = make_array(inner, 3);
    CHECK_EQ(outer->size(), 24);
    CHECK_EQ(outer->array_length, 3);
}

TEST(type_array_of_pointers) {
    auto a = make_array(make_pointer(make_type(TypeKind::Char)), 5);
    CHECK_EQ(a->size(), 10);
}

TEST(type_class_size_comes_from_layout) {
    clear_class_sizes();
    auto c = make_type(TypeKind::Class);
    c->class_name = "Blinker";
    // Unlaid-out classes report zero rather than guessing.
    CHECK_EQ(c->size(), 0);
    set_class_size("Blinker", 7);
    CHECK_EQ(c->size(), 7);
    CHECK_EQ(make_array(c, 3)->size(), 21);
    clear_class_sizes();
}

TEST(type_pointer_to_class_is_still_two_bytes) {
    clear_class_sizes();
    auto c = make_type(TypeKind::Class);
    c->class_name = "Big";
    set_class_size("Big", 200);
    CHECK_EQ(c->size(), 200);
    CHECK_EQ(make_pointer(c)->size(), 2);
    clear_class_sizes();
}
