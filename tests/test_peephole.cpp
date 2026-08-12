// Tests for the AVR peephole optimiser.
//
// Every positive test checks two things: that the rewrite happened, and that
// the rewritten text still assembles. A pass that produces smaller code the
// assembler rejects is worse than no pass at all.
//
// The negative tests matter more than the positive ones. They pin down the
// cases the optimiser must refuse: a saved register that the span in between
// actually clobbers, a window that crosses a label, a flag-setting instruction
// a branch depends on, and a skip instruction whose target must not move.

#include "harness.h"

#include "ardio/avr/assembler.h"
#include "ardio/avr/peephole.h"

#include <string>

using ardio::assemble;
using ardio::optimise_assembly;

namespace {

// Number of times `needle` appears in `text`.
int count(const std::string& text, const std::string& needle) {
    int n = 0;
    for (size_t i = text.find(needle); i != std::string::npos;
         i = text.find(needle, i + needle.size()))
        ++n;
    return n;
}

bool has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// Assembles and reports the size in bytes, or -1 if it does not assemble.
int flash_size(const std::string& source) {
    ardio::AssembleResult r = assemble(source);
    if (!r.ok) return -1;
    return int(r.code.size());
}

// Runs the pass and checks the result still assembles, returning it.
std::string optimised(const std::string& source) {
    std::string out = optimise_assembly(source);
    if (flash_size(out) < 0)
        ::ardio_test::fail(__FILE__, __LINE__,
                           "optimised output does not assemble: " + assemble(out).error +
                               "\n--- output ---\n" + out);
    return out;
}

} // namespace

// ---------------------------------------------------------------- basics ---

TEST(peephole_leaves_clean_code_alone) {
    const std::string src =
        "start:\n"
        "    ldi r24, 5\n"
        "    ldi r25, 0\n"
        "    add r24, r25\n"
        "    ret\n";
    CHECK(optimised(src) == src);
}

TEST(peephole_preserves_comments_and_directives) {
    const std::string src =
        "; a comment\n"
        ".equ SPEED, 3\n"
        "\n"
        "    ldi r24, SPEED\n"
        "    ret\n";
    CHECK(optimised(src) == src);
}

TEST(peephole_handles_empty_and_newline_only_input) {
    CHECK(optimise_assembly("") == "");
    CHECK(optimise_assembly("\n") == "\n");
    CHECK(optimise_assembly("    ret") == "    ret");
    CHECK(optimise_assembly("    ret\n") == "    ret\n");
}

// ------------------------------------------------------------- self moves ---

TEST(peephole_drops_self_move) {
    const std::string out = optimised(
        "    ldi r24, 1\n"
        "    mov r24, r24\n"
        "    ret\n");
    CHECK(!has(out, "mov"));
    CHECK(has(out, "ldi r24, 1"));
    CHECK(flash_size(out) == 4);
}

TEST(peephole_keeps_real_move) {
    const std::string out = optimised(
        "    ldi r24, 1\n"
        "    mov r18, r24\n"
        "    ret\n");
    CHECK(has(out, "mov r18, r24"));
}

// -------------------------------------------------------------- push/pop ---

TEST(peephole_drops_adjacent_push_pop) {
    const std::string out = optimised(
        "    ldi r24, 7\n"
        "    push r24\n"
        "    pop r24\n"
        "    ret\n");
    CHECK(!has(out, "push"));
    CHECK(!has(out, "pop"));
    CHECK(flash_size(out) == 4);
}

TEST(peephole_drops_push_pop_over_untouched_span) {
    const std::string out = optimised(
        "    push r24\n"
        "    ldi r18, 3\n"
        "    add r18, r19\n"
        "    std Y+1, r18\n"
        "    pop r24\n"
        "    ret\n");
    CHECK(!has(out, "push"));
    CHECK(!has(out, "pop"));
    CHECK(has(out, "ldi r18, 3"));
    CHECK(has(out, "std Y+1, r18"));
}

TEST(peephole_keeps_push_pop_when_span_writes_the_register) {
    const std::string src =
        "    push r24\n"
        "    ldi r24, 9\n"
        "    pop r24\n"
        "    ret\n";
    const std::string out = optimised(src);
    CHECK(has(out, "push r24"));
    CHECK(has(out, "pop r24"));
}

TEST(peephole_keeps_push_pop_across_a_call) {
    // The call may clobber r24, and it also moves the stack.
    const std::string out = optimised(
        "    push r24\n"
        "    call work\n"
        "    pop r24\n"
        "    ret\n"
        "work:\n"
        "    ret\n");
    CHECK(has(out, "push r24"));
    CHECK(has(out, "pop r24"));
}

TEST(peephole_keeps_push_pop_across_a_label) {
    // Control can enter at the label with r24 holding anything, and can also
    // reach the pop without having run the push.
    const std::string out = optimised(
        "    push r24\n"
        "    ldi r18, 1\n"
        "again:\n"
        "    dec r18\n"
        "    pop r24\n"
        "    ret\n");
    CHECK(has(out, "push r24"));
    CHECK(has(out, "pop r24"));
}

TEST(peephole_keeps_push_pop_around_a_nested_push) {
    // An unbalanced push in between means the pop no longer sees our value.
    const std::string out = optimised(
        "    push r24\n"
        "    push r18\n"
        "    pop r24\n"
        "    ret\n");
    CHECK(count(out, "push") == 2);
    CHECK(count(out, "pop") == 1);
}

TEST(peephole_keeps_push_pop_of_different_registers) {
    const std::string out = optimised(
        "    push r24\n"
        "    pop r18\n"
        "    ret\n");
    CHECK(has(out, "push r24"));
    CHECK(has(out, "pop r18"));
}

TEST(peephole_keeps_push_pop_across_an_unknown_instruction) {
    // An instruction the optimiser has no facts about must be assumed to
    // clobber everything.
    const std::string out = optimised(
        "    push r24\n"
        "    lpm\n"
        "    icall\n"
        "    pop r24\n"
        "    ret\n");
    CHECK(has(out, "push r24"));
    CHECK(has(out, "pop r24"));
}

// -------------------------------------------------------- frame slot loads ---

TEST(peephole_drops_load_after_matching_store) {
    const std::string out = optimised(
        "    std Y+4, r24\n"
        "    ldd r24, Y+4\n"
        "    ret\n");
    CHECK(has(out, "std Y+4, r24"));
    CHECK(!has(out, "ldd"));
    CHECK(flash_size(out) == 4);
}

TEST(peephole_keeps_load_of_a_different_slot) {
    const std::string out = optimised(
        "    std Y+4, r24\n"
        "    ldd r24, Y+5\n"
        "    ret\n");
    CHECK(has(out, "ldd r24, Y+5"));
}

TEST(peephole_keeps_load_into_a_different_register) {
    const std::string out = optimised(
        "    std Y+4, r24\n"
        "    ldd r18, Y+4\n"
        "    ret\n");
    CHECK(has(out, "ldd r18, Y+4"));
}

TEST(peephole_keeps_load_after_store_across_a_label) {
    const std::string out = optimised(
        "    std Y+4, r24\n"
        "top:\n"
        "    ldd r24, Y+4\n"
        "    ret\n");
    CHECK(has(out, "ldd r24, Y+4"));
}

TEST(peephole_leaves_z_relative_loads_alone) {
    // Z is a general pointer and could address a memory-mapped register, where
    // reading twice is not the same as reading once.
    const std::string out = optimised(
        "    std Z+2, r24\n"
        "    ldd r24, Z+2\n"
        "    ldd r24, Z+2\n"
        "    ret\n");
    CHECK(count(out, "ldd r24, Z+2") == 2);
}

TEST(peephole_drops_repeated_load) {
    const std::string out = optimised(
        "    ldd r24, Y+2\n"
        "    ldd r24, Y+2\n"
        "    ret\n");
    CHECK(count(out, "ldd") == 1);
    CHECK(flash_size(out) == 4);
}

TEST(peephole_keeps_repeated_load_with_a_write_between) {
    const std::string out = optimised(
        "    ldd r24, Y+2\n"
        "    std Y+2, r18\n"
        "    ldd r24, Y+2\n"
        "    ret\n");
    CHECK(count(out, "ldd r24, Y+2") == 2);
}

// --------------------------------------------------------------- movw ---

TEST(peephole_folds_register_pair_move_into_movw) {
    const std::string out = optimised(
        "    mov r24, r22\n"
        "    mov r25, r23\n"
        "    ret\n");
    CHECK(has(out, "movw r24, r22"));
    CHECK(!has(out, "mov r24"));
    CHECK(flash_size(out) == 4);
}

TEST(peephole_folds_register_pair_move_written_high_half_first) {
    const std::string out = optimised(
        "    mov r19, r25\n"
        "    mov r18, r24\n"
        "    ret\n");
    CHECK(has(out, "movw r18, r24"));
    CHECK(count(out, "mov ") == 0);
}

TEST(peephole_keeps_unaligned_pair_moves) {
    // r23:r24 is not a MOVW pair -- MOVW needs even register numbers.
    const std::string out = optimised(
        "    mov r23, r21\n"
        "    mov r24, r22\n"
        "    ret\n");
    CHECK(!has(out, "movw"));
    CHECK(count(out, "mov ") == 2);
}

TEST(peephole_keeps_unrelated_consecutive_moves) {
    const std::string out = optimised(
        "    mov r24, r22\n"
        "    mov r18, r20\n"
        "    ret\n");
    CHECK(!has(out, "movw"));
}

TEST(peephole_keeps_pair_moves_split_by_a_label) {
    const std::string out = optimised(
        "    mov r24, r22\n"
        "mid:\n"
        "    mov r25, r23\n"
        "    ret\n");
    CHECK(!has(out, "movw"));
}

// ----------------------------------------------------------- tail calls ---

TEST(peephole_turns_call_then_ret_into_a_jump) {
    const std::string src =
        "    call work\n"
        "    ret\n"
        "other:\n"
        "    ldi r24, 2\n"
        "    ret\n"
        "work:\n"
        "    ldi r24, 1\n"
        "    ret\n";
    const std::string out = optimised(src);
    CHECK(has(out, "rjmp work"));
    CHECK(!has(out, "call"));
    CHECK(flash_size(out) < flash_size(src));
}

TEST(peephole_turns_rcall_then_ret_into_a_jump) {
    const std::string out = optimised(
        "    rcall work\n"
        "    ret\n"
        "other:\n"
        "    ret\n"
        "work:\n"
        "    ret\n");
    CHECK(has(out, "rjmp work"));
    CHECK(!has(out, "rcall"));
}

TEST(peephole_tail_call_to_the_next_function_falls_through) {
    // Once the CALL becomes an RJMP to the very next label, even the jump goes.
    const std::string src =
        "    call work\n"
        "    ret\n"
        "work:\n"
        "    ret\n";
    const std::string out = optimised(src);
    CHECK(!has(out, "call"));
    CHECK(!has(out, "rjmp"));
    CHECK(has(out, "work:"));
    CHECK(flash_size(out) < flash_size(src));
}

TEST(peephole_keeps_call_when_the_ret_is_a_jump_target) {
    // Something else can branch to `done`, so the RET has to stay -- and with
    // it the CALL, whose return address it consumes.
    const std::string out = optimised(
        "    call work\n"
        "done:\n"
        "    ret\n"
        "work:\n"
        "    ret\n");
    CHECK(has(out, "call work"));
    CHECK(count(out, "ret") == 2);
}

TEST(peephole_keeps_call_that_a_skip_may_jump_over) {
    // SBRC skips exactly one instruction. Folding the CALL and RET together
    // would change where the skip lands.
    const std::string out = optimised(
        "    sbrc r24, 0\n"
        "    call work\n"
        "    ret\n"
        "work:\n"
        "    ret\n");
    CHECK(has(out, "call work"));
}

TEST(peephole_keeps_call_to_a_numeric_address) {
    // The assembler reads a bare number after RJMP as an offset but after CALL
    // as an address, so this one may not be rewritten.
    const std::string out = optimised(
        "    call 0x40\n"
        "    ret\n");
    CHECK(has(out, "call 0x40"));
}

// ------------------------------------------------------- redundant jumps ---

TEST(peephole_drops_jump_to_the_next_line) {
    const std::string out = optimised(
        "    ldi r24, 1\n"
        "    rjmp next\n"
        "next:\n"
        "    ret\n");
    CHECK(!has(out, "rjmp"));
    CHECK(has(out, "next:"));
    CHECK(flash_size(out) == 4);
}

TEST(peephole_drops_jump_over_intervening_labels) {
    const std::string out = optimised(
        "    rjmp second\n"
        "first:\n"
        "second:\n"
        "    ret\n");
    CHECK(!has(out, "rjmp"));
    CHECK(has(out, "first:"));
    CHECK(has(out, "second:"));
}

TEST(peephole_keeps_jump_past_real_code) {
    const std::string out = optimised(
        "    rjmp after\n"
        "mid:\n"
        "    ldi r24, 1\n"
        "after:\n"
        "    ret\n");
    CHECK(has(out, "rjmp after"));
    CHECK(has(out, "ldi r24, 1"));
}

TEST(peephole_keeps_jump_that_a_skip_may_jump_over) {
    const std::string out = optimised(
        "    sbrs r24, 1\n"
        "    rjmp next\n"
        "next:\n"
        "    ret\n");
    CHECK(has(out, "rjmp next"));
}

TEST(peephole_keeps_conditional_branch_to_the_next_line) {
    // A branch is not an unconditional jump; leave it be.
    const std::string out = optimised(
        "    cp r24, r25\n"
        "    breq next\n"
        "next:\n"
        "    ret\n");
    CHECK(has(out, "breq next"));
}

// -------------------------------------------------------- unreachable code ---

TEST(peephole_drops_code_after_ret) {
    const std::string out = optimised(
        "    ret\n"
        "    ldi r24, 1\n"
        "    ldi r25, 2\n"
        "after:\n"
        "    ret\n");
    CHECK(!has(out, "ldi"));
    CHECK(has(out, "after:"));
    CHECK(flash_size(out) == 4);
}

TEST(peephole_drops_code_after_rjmp) {
    const std::string out = optimised(
        "    rjmp away\n"
        "    ldi r24, 1\n"
        "away:\n"
        "    ret\n");
    CHECK(!has(out, "ldi"));
}

TEST(peephole_keeps_code_after_a_label) {
    const std::string out = optimised(
        "    ret\n"
        "live:\n"
        "    ldi r24, 1\n"
        "    ret\n");
    CHECK(has(out, "ldi r24, 1"));
}

TEST(peephole_keeps_instruction_a_skip_jumps_to) {
    // SBRC skips the RET, landing exactly on the LDI. That LDI is reachable.
    const std::string out = optimised(
        "    sbrc r24, 0\n"
        "    ret\n"
        "    ldi r24, 1\n"
        "    ret\n");
    CHECK(has(out, "ldi r24, 1"));
}

TEST(peephole_stops_unreachable_removal_at_a_directive) {
    const std::string out = optimised(
        "    ret\n"
        ".byte 1, 2, 3\n"
        "table:\n"
        "    ret\n");
    CHECK(has(out, ".byte 1, 2, 3"));
}

// ------------------------------------------------------------ flag safety ---

TEST(peephole_keeps_flag_setter_before_a_branch) {
    // Nothing here may be removed: the CP sets the flags BREQ reads, and the
    // MOV feeding it is a real move.
    const std::string src =
        "    ldd r24, Y+1\n"
        "    mov r18, r24\n"
        "    cp r18, r19\n"
        "    breq equal\n"
        "    ldi r24, 0\n"
        "equal:\n"
        "    ret\n";
    CHECK(optimised(src) == src);
}

TEST(peephole_keeps_flag_setter_between_a_push_and_pop) {
    // The push/pop pair goes away, but the CPI that BRNE depends on stays --
    // removing PUSH and POP cannot disturb the flags, since neither writes
    // SREG.
    const std::string out = optimised(
        "    push r24\n"
        "    cpi r18, 3\n"
        "    brne skip\n"
        "    ldi r18, 0\n"
        "skip:\n"
        "    pop r24\n"
        "    ret\n");
    CHECK(has(out, "cpi r18, 3"));
    CHECK(has(out, "brne skip"));
    // The label between them also blocks the push/pop rewrite.
    CHECK(has(out, "push r24"));
    CHECK(has(out, "pop r24"));
}

// ----------------------------------------------------------- interactions ---

TEST(peephole_runs_to_a_fixed_point) {
    // Removing the POP makes the two MOVs adjacent, which then folds to a
    // MOVW: a second round finds what the first exposed.
    const std::string out = optimised(
        "    push r20\n"
        "    mov r24, r22\n"
        "    pop r20\n"
        "    mov r25, r23\n"
        "    ret\n");
    CHECK(has(out, "movw r24, r22"));
    CHECK(!has(out, "push"));
    CHECK(!has(out, "pop"));
    CHECK(flash_size(out) == 4);
}

TEST(peephole_keeps_labels_of_deleted_instructions) {
    // The MOV is a no-op, but `here` is a jump target and must survive.
    const std::string out = optimised(
        "    rjmp here\n"
        "here:  mov r24, r24\n"
        "    ret\n");
    CHECK(has(out, "here:"));
    CHECK(flash_size(out) == 2);
    CHECK(assemble(out).ok);
}

TEST(peephole_shrinks_a_representative_function) {
    // The shape the code generator produces for `int f(int a) { return a; }`
    // with a spilled parameter and a saved-around subexpression.
    const std::string src =
        "f:\n"
        "    push r28\n"
        "    push r29\n"
        "    std Y+1, r24\n"
        "    ldd r24, Y+1\n"
        "    ldd r24, Y+1\n"
        "    push r18\n"
        "    mov r22, r24\n"
        "    pop r18\n"
        "    mov r23, r25\n"
        "    mov r24, r24\n"
        "    call helper\n"
        "    ret\n"
        "    ldi r24, 0\n"
        "helper:\n"
        "    pop r29\n"
        "    pop r28\n"
        "    ret\n";
    const std::string out = optimised(src);
    int before = flash_size(src), after = flash_size(out);
    CHECK(before > 0);
    CHECK(after > 0);
    CHECK(after < before);
    // push r28 / push r29 survive: the pops are behind a label.
    CHECK(has(out, "push r28"));
    CHECK(has(out, "movw r22, r24"));
    // The CALL becomes a tail call, the dead LDI after the RET goes, and the
    // resulting jump lands on the next label -- so it falls through instead.
    CHECK(!has(out, "call"));
    CHECK(count(out, "ldd") == 0);
    CHECK(!has(out, "ldi r24, 0"));
}
