#include "harness.h"
#include "ardio/avr/assembler.h"
#include <string>

namespace {

// Assembles `src` and returns the little-endian word at word index `i`.
// AVR is little-endian: the low byte of each 16-bit instruction comes first.
int word_at(const ardio::AssembleResult& r, size_t i) {
    if (r.code.size() < (i + 1) * 2) return -1;
    return int(r.code[i * 2]) | (int(r.code[i * 2 + 1]) << 8);
}

ardio::AssembleResult asm_ok(const char* src) {
    auto r = ardio::assemble(src);
    if (!r.ok) std::printf("    (assembler said: %s)\n", r.error.c_str());
    return r;
}

} // namespace

TEST(avr_encodes_nop) {
    auto r = asm_ok("nop");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0x0000);
}

TEST(avr_encodes_ldi_immediate_into_upper_registers) {
    // ldi r16, 0xFF  ->  0xEF0F
    auto r = asm_ok("ldi r16, 0xFF");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0xEF0F);
}

TEST(avr_ldi_rejects_lower_half_registers) {
    // LDI can only target r16-r31; there is no encoding for r0-r15.
    auto r = ardio::assemble("ldi r15, 1");
    CHECK(!r.ok);
    CHECK(r.error.find("r16") != std::string::npos);
}

TEST(avr_encodes_out_to_io_space) {
    // out 0x05, r16  ->  0xB905   (0x05 = PORTB)
    auto r = asm_ok("out 0x05, r16");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0xB905);
}

TEST(avr_encodes_sbi_and_cbi) {
    // sbi 0x04, 5  ->  0x9A25     (DDRB, bit 5)
    // cbi 0x05, 5  ->  0x982D     (PORTB, bit 5)
    auto r = asm_ok("sbi 0x04, 5\ncbi 0x05, 5");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0x9A25);
    CHECK_EQ(word_at(r, 1), 0x982D);
}

TEST(avr_encodes_dec) {
    // dec r16  ->  0x950A
    auto r = asm_ok("dec r16");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0x950A);
}

TEST(avr_rjmp_to_itself_is_minus_one) {
    // "loop: rjmp loop" jumps back over itself: offset -1 word -> 0xCFFF
    auto r = asm_ok("loop: rjmp loop");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0xCFFF);
}

TEST(avr_rjmp_forward_over_one_instruction) {
    // rjmp skip / nop / skip: nop   -> offset +1 -> 0xC001
    auto r = asm_ok("rjmp skip\nnop\nskip: nop");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0xC001);
}

TEST(avr_brne_backwards_one_word) {
    // back: brne back  -> offset -1 -> 0xF7F9
    auto r = asm_ok("back: brne back");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0xF7F9);
}

TEST(avr_ignores_comments_and_blank_lines) {
    auto r = asm_ok("; a comment\n\n  nop  ; trailing\n");
    CHECK(r.ok);
    CHECK_EQ(r.code.size(), size_t(2));
    CHECK_EQ(word_at(r, 0), 0x0000);
}

TEST(avr_accepts_labels_on_their_own_line) {
    auto r = asm_ok("start:\n    nop\n    rjmp start");
    CHECK(r.ok);
    CHECK_EQ(r.code.size(), size_t(4));
    CHECK_EQ(word_at(r, 1), 0xCFFE);  // back over nop and itself: -2
}

TEST(avr_reports_unknown_mnemonic_with_line_number) {
    auto r = ardio::assemble("nop\nfhqwhgads r1");
    CHECK(!r.ok);
    CHECK(r.error.find("line 2") != std::string::npos);
    CHECK(r.error.find("fhqwhgads") != std::string::npos);
}

TEST(avr_reports_undefined_label) {
    auto r = ardio::assemble("rjmp nowhere");
    CHECK(!r.ok);
    CHECK(r.error.find("nowhere") != std::string::npos);
}

TEST(avr_reports_out_of_range_io_address) {
    // OUT addresses only 0x00-0x3F.
    auto r = ardio::assemble("out 0x40, r16");
    CHECK(!r.ok);
    CHECK(r.error.find("range") != std::string::npos);
}

TEST(avr_relaxes_a_branch_that_cannot_reach) {
    // BRNE reaches only +63/-64 words, so a target 200 words back is out of
    // reach. Rather than failing, the assembler inverts the condition to skip
    // over an RJMP, which reaches +2047/-2048:
    //     brne back   ->   breq .+2 ; rjmp back
    std::string src = "back:\n";
    for (int i = 0; i < 200; ++i) src += "nop\n";
    src += "brne back\n";

    auto r = asm_ok(src.c_str());
    CHECK(r.ok);
    // 200 nops plus a two-word relaxed branch.
    CHECK_EQ(r.code.size(), size_t(200 * 2 + 4));

    // breq (0xF001) skipping one word: 0xF001 | (1 << 3) = 0xF009
    CHECK_EQ(word_at(r, 200), 0xF009);
    // rjmp back from word 201: 201 + 1 + k = 0 -> k = -202
    CHECK_EQ(word_at(r, 201), int(0xC000 | (unsigned(-202) & 0x0FFF)));
}

TEST(avr_relaxation_keeps_later_labels_correct) {
    // Relaxing makes the branch two words instead of one, so anything after it
    // shifts. A label placed after a relaxed branch must still resolve.
    std::string src = "back:\n";
    for (int i = 0; i < 100; ++i) src += "nop\n";
    src += "brne back\n";
    src += "after: rjmp after\n";

    auto r = asm_ok(src.c_str());
    CHECK(r.ok);
    // 100 nops + 2 relaxed-branch words = word 102 for "after".
    CHECK_EQ(word_at(r, 102), 0xCFFF);   // rjmp to itself
}

TEST(avr_reports_a_branch_that_even_rjmp_cannot_reach) {
    // RJMP reaches +2047/-2048 words. Past that there is nothing left to try.
    std::string src = "back:\n";
    for (int i = 0; i < 3000; ++i) src += "nop\n";
    src += "brne back\n";
    auto r = ardio::assemble(src);
    CHECK(!r.ok);
    CHECK(r.error.find("range") != std::string::npos);
}

TEST(avr_assembles_a_complete_blink_program) {
    // The real thing: set PB5 as output, then toggle it forever.
    const char* src =
        "        sbi  0x04, 5      ; DDRB  |= (1<<5)  -- pin 13 output\n"
        "loop:   sbi  0x05, 5      ; PORTB |= (1<<5)  -- LED on\n"
        "        cbi  0x05, 5      ; PORTB &= ~(1<<5) -- LED off\n"
        "        rjmp loop\n";
    auto r = asm_ok(src);
    CHECK(r.ok);
    CHECK_EQ(r.code.size(), size_t(8));   // four 16-bit instructions
    CHECK_EQ(word_at(r, 0), 0x9A25);
    CHECK_EQ(word_at(r, 1), 0x9A2D);
    CHECK_EQ(word_at(r, 2), 0x982D);
    CHECK_EQ(word_at(r, 3), 0xCFFD);      // back 3 words to loop
}

TEST(avr_encodes_movw_register_pairs) {
    // movw r24, r22  ->  0x01CB
    auto r = asm_ok("movw r24, r22");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0x01CB);
}

TEST(avr_movw_rejects_odd_registers) {
    auto r = ardio::assemble("movw r25, r22");
    CHECK(!r.ok);
    CHECK(r.error.find("even") != std::string::npos);
}

TEST(avr_encodes_adiw_and_sbiw) {
    // adiw r24, 1 -> 0x9601 ; sbiw r28, 2 -> 0x9722
    auto r = asm_ok("adiw r24, 1\nsbiw r28, 2");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0x9601);
    CHECK_EQ(word_at(r, 1), 0x9722);
}

TEST(avr_adiw_rejects_non_pair_registers) {
    auto r = ardio::assemble("adiw r23, 1");
    CHECK(!r.ok);
    CHECK(r.error.find("r24") != std::string::npos);
}

TEST(avr_lds_and_sts_occupy_two_words) {
    // lds r24, 0x0100 -> 0x9180 0x0100
    auto r = asm_ok("lds r24, 0x0100");
    CHECK(r.ok);
    CHECK_EQ(r.code.size(), size_t(4));
    CHECK_EQ(word_at(r, 0), 0x9180);
    CHECK_EQ(word_at(r, 1), 0x0100);
}

TEST(avr_two_word_instructions_shift_later_labels) {
    // lds is 2 words, so "here" sits at word 2, and rjmp here is +0... but the
    // rjmp itself is at word 2, so the offset back to itself is -1.
    auto r = asm_ok("lds r24, 0x0100\nhere: rjmp here");
    CHECK(r.ok);
    CHECK_EQ(r.code.size(), size_t(6));
    CHECK_EQ(word_at(r, 2), 0xCFFF);
}

TEST(avr_call_is_two_words_with_absolute_target) {
    auto r = asm_ok("call target\nnop\ntarget: ret");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0x940E);
    CHECK_EQ(word_at(r, 1), 0x0003);   // call=2 words, nop=1 -> target at 3
}

TEST(avr_encodes_std_and_ldd_with_displacement) {
    // Layout is 10q0 qq0d dddd 1qqq (bit 9 selects store, bit 3 selects Y).
    // r24 -> d4=0x0100 plus low nibble 0x0080; q=3 -> 0x0003.
    //   ldd r24, Y+3 -> 0x818B
    //   std Y+3, r24 -> 0x838B
    auto r = asm_ok("std Y+3, r24\nldd r24, Y+3");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0x838B);
    CHECK_EQ(word_at(r, 1), 0x818B);
}

TEST(avr_clr_is_an_alias_for_eor_with_itself) {
    // clr r24 == eor r24, r24 -> 0x2788
    auto r = asm_ok("clr r24");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0x2788);
}

TEST(avr_encodes_push_pop_and_ret) {
    auto r = asm_ok("push r28\npop r28\nret");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0x93CF);
    CHECK_EQ(word_at(r, 1), 0x91CF);
    CHECK_EQ(word_at(r, 2), 0x9508);
}

TEST(avr_encodes_ld_and_st_pointer_modes) {
    // ld r24, X -> 0x918C ; ld r24, X+ -> 0x918D ; ld r24, -X -> 0x918E
    auto r = asm_ok("ld r24, X\nld r24, X+\nld r24, -X");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0x918C);
    CHECK_EQ(word_at(r, 1), 0x918D);
    CHECK_EQ(word_at(r, 2), 0x918E);
}

TEST(avr_encodes_st_through_z_with_increment) {
    // st Z+, r24 -> 0x9381
    auto r = asm_ok("st Z+, r24");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0x9381);
}

TEST(avr_plain_y_and_z_loads_match_zero_displacement) {
    // ld r24, Y == ldd r24, Y+0 ; ld r24, Z == ldd r24, Z+0
    auto a = asm_ok("ld r24, Y\nld r24, Z");
    auto b = asm_ok("ldd r24, Y+0\nldd r24, Z+0");
    CHECK(a.ok && b.ok);
    CHECK_EQ(word_at(a, 0), word_at(b, 0));
    CHECK_EQ(word_at(a, 1), word_at(b, 1));
}

TEST(avr_rejects_malformed_pointer_operand) {
    auto r = ardio::assemble("ld r24, W+");
    CHECK(!r.ok);
}

TEST(avr_encodes_sreg_bit_instructions) {
    auto r = asm_ok("sec\nclc\nset\nclt\nlpm\nicall\nijmp");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0x9408);  // sec
    CHECK_EQ(word_at(r, 1), 0x9488);  // clc
    CHECK_EQ(word_at(r, 2), 0x9468);  // set
    CHECK_EQ(word_at(r, 3), 0x94E8);  // clt
    CHECK_EQ(word_at(r, 4), 0x95C8);  // lpm
    CHECK_EQ(word_at(r, 5), 0x9509);  // icall
    CHECK_EQ(word_at(r, 6), 0x9409);  // ijmp
}

TEST(avr_encodes_bst_bld_and_cpse) {
    // r24 contributes (24<<4) = 0x180, which includes the d4 bit at 0x100.
    //   bst  r24, 3   -> 0xFA00 | 0x180 | 3            = 0xFB83
    //   bld  r24, 3   -> 0xF800 | 0x180 | 3            = 0xF983
    //   cpse r24, r22 -> 0x1000 | 0x200 | 0x180 | 6    = 0x1386
    //                    (r22's bit 4 lands at 0x200)
    auto r = asm_ok("bst r24, 3\nbld r24, 3\ncpse r24, r22");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0xFB83);
    CHECK_EQ(word_at(r, 1), 0xF983);
    CHECK_EQ(word_at(r, 2), 0x1386);
}

TEST(avr_cbr_is_andi_with_the_complemented_mask) {
    // cbr r16, 0x0F == andi r16, 0xF0 -> 0x7F00
    auto a = asm_ok("cbr r16, 0x0F");
    auto b = asm_ok("andi r16, 0xF0");
    CHECK(a.ok && b.ok);
    CHECK_EQ(word_at(a, 0), word_at(b, 0));
}

// --------------------------------------------------------------- directives ---

namespace {
// Byte at index `i`, as an int so CHECK_EQ can print it.
int byte_at(const ardio::AssembleResult& r, size_t i) {
    return i < r.code.size() ? int(r.code[i]) : -1;
}
} // namespace

TEST(avr_equ_defines_a_constant_usable_as_an_immediate) {
    auto r = asm_ok(".equ MASK, 0xFF\nldi r16, MASK");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0xEF0F);
}

TEST(avr_equ_constant_works_as_an_io_address) {
    auto r = asm_ok(".equ PORTB, 0x05\nout PORTB, r16");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0xB905);
}

TEST(avr_set_can_redefine_a_constant_partway_through) {
    auto r = asm_ok(".set X, 1\nldi r16, X\n.set X, 2\nldi r16, X");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0xE001);
    CHECK_EQ(word_at(r, 1), 0xE002);
}

TEST(avr_constant_expressions_support_arithmetic_and_shifts) {
    // (3 << 2) | 1 == 13
    auto r = asm_ok(".equ N, 3\nldi r16, (N << 2) | 1");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0xE00D);
}

TEST(avr_org_pads_forward_with_erased_flash) {
    auto r = asm_ok("nop\n.org 3\nnop");
    CHECK(r.ok);
    CHECK_EQ(r.code.size(), size_t(8));
    CHECK_EQ(word_at(r, 1), 0xFFFF);
    CHECK_EQ(word_at(r, 2), 0xFFFF);
    CHECK_EQ(word_at(r, 3), 0x0000);
}

TEST(avr_org_rejects_moving_backwards) {
    auto r = ardio::assemble("nop\nnop\n.org 0");
    CHECK(!r.ok);
    CHECK(r.error.find("backwards") != std::string::npos);
}

TEST(avr_org_places_an_interrupt_vector_table) {
    // jmp is two words, so vector 1 starts at word 2.
    const char* src =
        "        jmp  main\n"
        "        .org 2\n"
        "        jmp  isr\n"
        "main:   nop\n"
        "isr:    reti\n";
    auto r = asm_ok(src);
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0x940C);
    CHECK_EQ(word_at(r, 1), 0x0004);   // main sits right after both vectors
    CHECK_EQ(word_at(r, 2), 0x940C);
    CHECK_EQ(word_at(r, 3), 0x0005);   // isr
}

TEST(avr_byte_emits_raw_bytes_and_pads_to_a_whole_word) {
    auto r = asm_ok(".byte 1, 2, 3");
    CHECK(r.ok);
    CHECK_EQ(r.code.size(), size_t(4));
    CHECK_EQ(byte_at(r, 0), 1);
    CHECK_EQ(byte_at(r, 2), 3);
    CHECK_EQ(byte_at(r, 3), 0xFF);     // pad to finish the word
}

TEST(avr_label_after_an_odd_byte_run_lands_on_the_next_word) {
    // The critical case: three bytes is one and a half words, so "here" has to
    // be rounded up to word 2, not left at word 1.5 or miscounted as word 3.
    auto r = asm_ok(".byte 1, 2, 3\nhere: nop\nrjmp here");
    CHECK(r.ok);
    CHECK_EQ(r.code.size(), size_t(8));
    CHECK_EQ(byte_at(r, 3), 0xFF);
    CHECK_EQ(word_at(r, 2), 0x0000);   // the nop
    CHECK_EQ(word_at(r, 3), 0xCFFE);   // rjmp back two words to "here"
}

TEST(avr_byte_rejects_values_that_do_not_fit) {
    auto r = ardio::assemble(".byte 300");
    CHECK(!r.ok);
    CHECK(r.error.find("range") != std::string::npos);
}

TEST(avr_word_emits_little_endian_words) {
    auto r = asm_ok(".word 0x1234, 5");
    CHECK(r.ok);
    CHECK_EQ(r.code.size(), size_t(4));
    CHECK_EQ(word_at(r, 0), 0x1234);
    CHECK_EQ(word_at(r, 1), 0x0005);
    CHECK_EQ(byte_at(r, 0), 0x34);     // low byte first
}

TEST(avr_ascii_and_asciz_differ_by_the_terminator) {
    auto a = asm_ok(".ascii \"hi\"");
    auto z = asm_ok(".asciz \"hi\"");
    CHECK(a.ok && z.ok);
    CHECK_EQ(a.code.size(), size_t(2));
    CHECK_EQ(byte_at(a, 0), 'h');
    CHECK_EQ(byte_at(a, 1), 'i');
    CHECK_EQ(z.code.size(), size_t(4));
    CHECK_EQ(byte_at(z, 2), 0);        // NUL
    CHECK_EQ(byte_at(z, 3), 0xFF);     // pad
}

TEST(avr_ascii_keeps_commas_and_comment_characters_inside_the_string) {
    auto r = asm_ok(".ascii \"a,b; c\"");
    CHECK(r.ok);
    CHECK_EQ(r.code.size(), size_t(6));
    CHECK_EQ(byte_at(r, 1), ',');
    CHECK_EQ(byte_at(r, 3), ';');
}

TEST(avr_ascii_decodes_escape_sequences) {
    auto r = asm_ok(".ascii \"a\\nb\\t\"");
    CHECK(r.ok);
    CHECK_EQ(r.code.size(), size_t(4));
    CHECK_EQ(byte_at(r, 1), '\n');
    CHECK_EQ(byte_at(r, 3), '\t');
}

TEST(avr_space_reserves_bytes_with_an_optional_fill) {
    auto r = asm_ok(".space 3, 0xAA\nnop");
    CHECK(r.ok);
    CHECK_EQ(r.code.size(), size_t(6));
    CHECK_EQ(byte_at(r, 0), 0xAA);
    CHECK_EQ(byte_at(r, 2), 0xAA);
    CHECK_EQ(byte_at(r, 3), 0xFF);     // pad before the instruction
    CHECK_EQ(word_at(r, 2), 0x0000);
}

TEST(avr_space_defaults_to_zero_fill) {
    auto r = asm_ok(".space 2");
    CHECK(r.ok);
    CHECK_EQ(r.code.size(), size_t(2));
    CHECK_EQ(byte_at(r, 0), 0);
    CHECK_EQ(byte_at(r, 1), 0);
}

TEST(avr_global_and_extern_are_accepted_and_ignored) {
    auto r = asm_ok(".global main\n.extern helper\nmain: nop");
    CHECK(r.ok);
    CHECK_EQ(r.code.size(), size_t(2));
    CHECK_EQ(word_at(r, 0), 0x0000);
}

TEST(avr_lo8_and_hi8_split_a_constant_into_two_immediates) {
    auto r = asm_ok(".equ ADDR, 0x1234\nldi r16, lo8(ADDR)\nldi r17, hi8(ADDR)");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0xE304);   // 0x34
    CHECK_EQ(word_at(r, 1), 0xE112);   // 0x12
}

TEST(avr_lo8_and_hi8_take_the_address_of_a_label) {
    // The usual way to point Z at a string: msg is at word 2.
    auto r = asm_ok("ldi r30, lo8(msg)\nldi r31, hi8(msg)\nmsg: .asciz \"hi\"");
    CHECK(r.ok);
    CHECK_EQ(word_at(r, 0), 0xE0E2);   // r30 <- 2
    CHECK_EQ(word_at(r, 1), 0xE0F0);   // r31 <- 0
    CHECK_EQ(byte_at(r, 4), 'h');
}

TEST(avr_reports_an_unknown_directive) {
    auto r = ardio::assemble(".frobnicate 1");
    CHECK(!r.ok);
    CHECK(r.error.find("directive") != std::string::npos);
}

TEST(avr_reports_an_undefined_constant_in_an_expression) {
    auto r = ardio::assemble("ldi r16, NOPE + 1");
    CHECK(!r.ok);
    CHECK(r.error.find("NOPE") != std::string::npos);
}

TEST(avr_data_between_two_routines_keeps_later_labels_correct) {
    // A five-byte string between two routines: the second routine still has to
    // start on a word boundary, and call must resolve to that word.
    const char* src =
        "        call second\n"
        "        ret\n"
        "msg:    .ascii \"hello\"\n"
        "second: ret\n";
    auto r = asm_ok(src);
    CHECK(r.ok);
    // call(2) + ret(1) = 3 words, then 5 bytes + 1 pad = 3 words -> second at 6.
    CHECK_EQ(word_at(r, 1), 0x0006);
    CHECK_EQ(byte_at(r, 11), 0xFF);
    CHECK_EQ(word_at(r, 6), 0x9508);
}
