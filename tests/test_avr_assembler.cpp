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

TEST(avr_reports_branch_out_of_range) {
    // BRNE reaches +63/-64 words. 200 nops puts the target far past that.
    std::string src = "back:\n";
    for (int i = 0; i < 200; ++i) src += "nop\n";
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
