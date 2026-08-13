#include "harness.h"
#include "ardio/board.h"
#include "ardio/platform/ports.h"
#include <string>

namespace {

// Signature bytes are the one field where a typo is silently destructive: too
// strict and ardio refuses a board that is perfectly fine, too loose and it
// happily programs a 168 image into a 328P. So each is checked against the
// datasheet value rather than against whatever the database happens to hold.
void check_signature(const char* id, int a, int b, int c) {
    const ardio::Board* board = ardio::find_board_by_id(id);
    CHECK(board != nullptr);
    CHECK_EQ(int(board->signature[0]), a);
    CHECK_EQ(int(board->signature[1]), b);
    CHECK_EQ(int(board->signature[2]), c);
}

ardio::PortInfo port_with(const char* dev, ardio::UsbId id) {
    return ardio::PortInfo{dev, "USB Serial", id, true};
}

} // namespace

// ---------------------------------------------------------------- lookup ----

TEST(board_database_contains_nano) {
    const ardio::Board* b = ardio::find_board_by_id("nano");
    CHECK(b != nullptr);
    CHECK(b->mcu == "atmega328p");
    CHECK_EQ(int(b->page_size), 128);
    CHECK_EQ(int(b->flash_size), 32768);
    CHECK(b->protocol == ardio::Protocol::Stk500v1);
}

TEST(every_expected_board_id_resolves) {
    for (const char* id : {"uno", "nano", "nano_old", "nano_168", "pro_mini",
                           "mega2560", "mega1280", "leonardo", "micro",
                           "esp8266"}) {
        const ardio::Board* b = ardio::find_board_by_id(id);
        CHECK(b != nullptr);
        CHECK(b->id == id);
    }
}

TEST(unknown_board_id_returns_null) {
    CHECK(ardio::find_board_by_id("teapot") == nullptr);
}

TEST(every_board_id_is_unique) {
    const auto& db = ardio::board_database();
    for (size_t i = 0; i < db.size(); ++i)
        for (size_t j = i + 1; j < db.size(); ++j)
            CHECK(db[i].id != db[j].id);
}

TEST(every_avr_board_declares_baud_rates_and_a_gcc_target) {
    for (const ardio::Board& b : ardio::board_database()) {
        CHECK(!b.baud_rates.empty());
        CHECK(b.flash_size > 0);
        CHECK(b.page_size > 0);
        CHECK(b.f_cpu > 0);
        if (b.protocol != ardio::Protocol::EspRom) CHECK(!b.gcc_mcu.empty());
    }
}

// ------------------------------------------------------------ signatures ----

TEST(atmega328p_boards_carry_the_328p_signature) {
    check_signature("uno", 0x1E, 0x95, 0x0F);
    check_signature("nano", 0x1E, 0x95, 0x0F);
    check_signature("nano_old", 0x1E, 0x95, 0x0F);
    check_signature("pro_mini", 0x1E, 0x95, 0x0F);
}

TEST(atmega168_signature) { check_signature("nano_168", 0x1E, 0x94, 0x06); }
TEST(atmega2560_signature) { check_signature("mega2560", 0x1E, 0x98, 0x01); }
TEST(atmega1280_signature) { check_signature("mega1280", 0x1E, 0x97, 0x03); }

TEST(atmega32u4_boards_carry_the_32u4_signature) {
    check_signature("leonardo", 0x1E, 0x95, 0x87);
    check_signature("micro", 0x1E, 0x95, 0x87);
}

TEST(esp8266_has_no_avr_signature) {
    check_signature("esp8266", 0x00, 0x00, 0x00);
}

// ------------------------------------------------------------ geometry -----

TEST(page_sizes_match_the_datasheets) {
    CHECK_EQ(int(ardio::find_board_by_id("uno")->page_size), 128);
    CHECK_EQ(int(ardio::find_board_by_id("nano_168")->page_size), 128);
    CHECK_EQ(int(ardio::find_board_by_id("leonardo")->page_size), 128);
    CHECK_EQ(int(ardio::find_board_by_id("micro")->page_size), 128);
    // The big AVRs double the page. A programmer that assumed 128 here would
    // write half of every page and leave the other half stale.
    CHECK_EQ(int(ardio::find_board_by_id("mega2560")->page_size), 256);
    CHECK_EQ(int(ardio::find_board_by_id("mega1280")->page_size), 256);
}

TEST(flash_sizes_match_the_datasheets) {
    CHECK_EQ(int(ardio::find_board_by_id("nano_168")->flash_size), 16384);
    CHECK_EQ(int(ardio::find_board_by_id("uno")->flash_size), 32768);
    CHECK_EQ(int(ardio::find_board_by_id("leonardo")->flash_size), 32768);
    CHECK_EQ(int(ardio::find_board_by_id("mega1280")->flash_size), 131072);
    CHECK_EQ(int(ardio::find_board_by_id("mega2560")->flash_size), 262144);
}

TEST(every_flash_size_is_a_whole_number_of_pages) {
    for (const ardio::Board& b : ardio::board_database())
        CHECK_EQ(int(b.flash_size % b.page_size), 0);
}

// --------------------------------------------------------------- clocks ----

TEST(pro_mini_runs_at_8mhz_unlike_the_uno) {
    const ardio::Board* pro = ardio::find_board_by_id("pro_mini");
    const ardio::Board* uno = ardio::find_board_by_id("uno");
    CHECK(pro != nullptr);
    CHECK(uno != nullptr);
    CHECK_EQ(pro->f_cpu, 8000000);
    CHECK_EQ(uno->f_cpu, 16000000);
    // Same silicon, same signature -- the difference is the crystal, which is
    // exactly why f_cpu cannot be derived from the MCU.
    CHECK(pro->mcu == uno->mcu);
    CHECK(pro->signature == uno->signature);
    CHECK(pro->f_cpu != uno->f_cpu);
}

// ------------------------------------------------------------ protocols ----

TEST(protocols_are_assigned_per_board) {
    CHECK(ardio::find_board_by_id("uno")->protocol == ardio::Protocol::Stk500v1);
    CHECK(ardio::find_board_by_id("nano_old")->protocol == ardio::Protocol::Stk500v1);
    CHECK(ardio::find_board_by_id("mega2560")->protocol == ardio::Protocol::Stk500v2);
    CHECK(ardio::find_board_by_id("mega1280")->protocol == ardio::Protocol::Stk500v2);
    CHECK(ardio::find_board_by_id("leonardo")->protocol == ardio::Protocol::Avr109);
    CHECK(ardio::find_board_by_id("micro")->protocol == ardio::Protocol::Avr109);
    CHECK(ardio::find_board_by_id("esp8266")->protocol == ardio::Protocol::EspRom);
}

TEST(old_bootloader_boards_do_not_offer_115200) {
    // The whole reason nano_old exists: the pre-2018 bootloader is silent at
    // 115200, so offering that rate at all would waste a sync attempt and, if
    // it were listed first, look like a dead board.
    const ardio::Board* b = ardio::find_board_by_id("nano_old");
    CHECK(b != nullptr);
    CHECK_EQ(b->baud_rates.size(), size_t(1));
    CHECK_EQ(b->baud_rates[0], 57600);
}

TEST(nano_tries_both_bootloader_baud_rates_new_first) {
    const ardio::Board* b = ardio::find_board_by_id("nano");
    CHECK(b != nullptr);
    CHECK_EQ(b->baud_rates.size(), size_t(2));
    CHECK_EQ(b->baud_rates[0], 115200);
    CHECK_EQ(b->baud_rates[1], 57600);
}

TEST(mega_variants_use_different_baud_rates) {
    CHECK_EQ(ardio::find_board_by_id("mega2560")->baud_rates[0], 115200);
    CHECK_EQ(ardio::find_board_by_id("mega1280")->baud_rates[0], 57600);
}

TEST(nano_168_uses_the_19200_bootloader) {
    CHECK_EQ(ardio::find_board_by_id("nano_168")->baud_rates[0], 19200);
}

// ---------------------------------------------------------- touch reset ----

TEST(only_the_32u4_boards_need_the_1200_baud_touch) {
    for (const ardio::Board& b : ardio::board_database()) {
        bool is_32u4 = b.mcu == "atmega32u4";
        CHECK_EQ(int(b.touch_reset_1200), int(is_32u4));
    }
}

// -------------------------------------------------------------- usb ids ----

TEST(ch340_usb_id_identifies_nano) {
    auto matches = ardio::find_boards_by_usb({0x1A86, 0x7523});
    CHECK_EQ(matches.size(), size_t(1));
    CHECK(matches[0]->id == "nano");
}

TEST(a_unique_usb_id_resolves_to_exactly_one_board) {
    auto mega = ardio::find_boards_by_usb({0x2341, 0x0042});
    CHECK_EQ(mega.size(), size_t(1));
    CHECK(mega[0]->id == "mega2560");

    auto leo = ardio::find_boards_by_usb({0x2341, 0x8036});
    CHECK_EQ(leo.size(), size_t(1));
    CHECK(leo[0]->id == "leonardo");
}

TEST(a_shared_usb_id_returns_more_than_one_board) {
    // 2341:0043 is claimed by Arduino's own board definitions for the Uno and
    // for the Nano alike. The database must not quietly pick one.
    auto matches = ardio::find_boards_by_usb({0x2341, 0x0043});
    CHECK(matches.size() > size_t(1));
    bool saw_uno = false, saw_nano = false;
    for (const ardio::Board* b : matches) {
        if (b->id == "uno") saw_uno = true;
        if (b->id == "nano") saw_nano = true;
    }
    CHECK(saw_uno);
    CHECK(saw_nano);
}

TEST(unknown_usb_id_matches_nothing) {
    auto matches = ardio::find_boards_by_usb({0xDEAD, 0xBEEF});
    CHECK_EQ(matches.size(), size_t(0));
}

TEST(leonardo_is_found_in_both_sketch_and_bootloader_mode) {
    const ardio::Board* leo = ardio::find_board_by_id("leonardo");
    CHECK(leo != nullptr);
    // Sketch mode and bootloader mode are different USB devices, and the
    // bootloader one is the mode we actually flash in. Both are recorded, and
    // both must resolve to the same board.
    CHECK_EQ(leo->usb_ids.size(), size_t(1));
    CHECK(leo->usb_ids[0] == (ardio::UsbId{0x2341, 0x8036}));
    CHECK_EQ(leo->bootloader_usb_ids.size(), size_t(1));
    CHECK(leo->bootloader_usb_ids[0] == (ardio::UsbId{0x2341, 0x0036}));

    auto sketch = ardio::find_boards_by_usb({0x2341, 0x8036});
    auto boot = ardio::find_boards_by_usb({0x2341, 0x0036});
    CHECK_EQ(sketch.size(), size_t(1));
    CHECK_EQ(boot.size(), size_t(1));
    CHECK(sketch[0] == leo);
    CHECK(boot[0] == leo);
}

TEST(micro_also_has_a_distinct_bootloader_id) {
    auto sketch = ardio::find_boards_by_usb({0x2341, 0x8037});
    auto boot = ardio::find_boards_by_usb({0x2341, 0x0037});
    CHECK_EQ(sketch.size(), size_t(1));
    CHECK_EQ(boot.size(), size_t(1));
    CHECK(sketch[0]->id == "micro");
    CHECK(boot[0]->id == "micro");
}

TEST(no_board_claims_another_boards_bootloader_id) {
    // A sketch-mode id colliding with some other board's bootloader id would
    // make the uploader think the reset had already happened.
    for (const ardio::Board& a : ardio::board_database())
        for (const ardio::UsbId& id : a.bootloader_usb_ids)
            for (const ardio::Board& b : ardio::board_database())
                if (&a != &b)
                    for (const ardio::UsbId& other : b.usb_ids)
                        CHECK(!(other == id));
}

// ------------------------------------------------------------ ambiguity ----

TEST(shared_bridge_ids_are_never_also_identifying_for_the_same_board) {
    // A board must not both claim an id and admit it is shared; that would
    // make find_boards_by_usb and boards_possible_for_usb disagree about it.
    for (const ardio::Board& b : ardio::board_database())
        for (const ardio::UsbId& shared : b.shared_usb_ids)
            for (const ardio::UsbId& own : b.usb_ids)
                CHECK(!(own == shared));
}

TEST(a_ch340_could_be_any_of_several_boards) {
    // Auto-detection settles on the Nano, but the truth is that a CH340 is
    // also how a clone Uno, an old-bootloader Nano, a 168 Nano and a Pro Mini
    // present themselves. That has to stay sayable, so the CLI can list the
    // alternatives instead of leaving the user guessing why the flash failed.
    auto possible = ardio::boards_possible_for_usb({0x1A86, 0x7523});
    CHECK(possible.size() > size_t(1));
    bool uno = false, nano = false, old = false, mini = false;
    for (const ardio::Board* b : possible) {
        if (b->id == "uno") uno = true;
        if (b->id == "nano") nano = true;
        if (b->id == "nano_old") old = true;
        if (b->id == "pro_mini") mini = true;
    }
    CHECK(uno);
    CHECK(nano);
    CHECK(old);
    CHECK(mini);
}

TEST(boards_possible_is_a_superset_of_boards_identified) {
    for (const ardio::UsbId id : {ardio::UsbId{0x1A86, 0x7523},
                                  ardio::UsbId{0x0403, 0x6001},
                                  ardio::UsbId{0x2341, 0x0043},
                                  ardio::UsbId{0x2341, 0x8036},
                                  ardio::UsbId{0xDEAD, 0xBEEF}}) {
        auto identified = ardio::find_boards_by_usb(id);
        auto possible = ardio::boards_possible_for_usb(id);
        CHECK(possible.size() >= identified.size());
        for (const ardio::Board* b : identified) {
            bool found = false;
            for (const ardio::Board* p : possible)
                if (p == b) found = true;
            CHECK(found);
        }
    }
}

TEST(a_usb_only_shared_by_boards_identifies_none_of_them) {
    // The Pro Mini has no USB hardware at all, so nothing about the adapter
    // clipped to its header can name it. Better to say so than to guess and
    // flash 16 MHz timing into an 8 MHz board.
    const ardio::Board* mini = ardio::find_board_by_id("pro_mini");
    CHECK(mini != nullptr);
    CHECK_EQ(mini->usb_ids.size(), size_t(0));
    CHECK_EQ(mini->bootloader_usb_ids.size(), size_t(0));
    CHECK(mini->shared_usb_ids.size() > size_t(0));
}

TEST(an_ambiguous_port_refuses_to_pick_and_says_to_pass_board) {
    // The consequence that actually matters: two boards on one id must end in
    // a message, not a flash. Naming both is the point -- the user cannot
    // choose if we do not tell them what the choices were.
    std::vector<ardio::PortInfo> ports{
        port_with("/dev/cu.usbmodem1401", {0x2341, 0x0043})};
    auto sel = ardio::select_port(ports, "", "");
    CHECK(sel.board == nullptr);
    CHECK(!sel.error.empty());
    CHECK(sel.error.find("--board") != std::string::npos);
    CHECK(sel.error.find("uno") != std::string::npos);
    CHECK(sel.error.find("nano") != std::string::npos);
}

TEST(naming_the_board_explicitly_resolves_an_ambiguous_port) {
    std::vector<ardio::PortInfo> ports{
        port_with("/dev/cu.usbmodem1401", {0x2341, 0x0043})};
    auto sel = ardio::select_port(ports, "", "uno");
    CHECK(sel.error.empty());
    CHECK(sel.board != nullptr);
    CHECK(sel.board->id == "uno");
    CHECK(sel.port != nullptr);
}

TEST(an_unambiguous_port_still_auto_detects) {
    std::vector<ardio::PortInfo> ports{
        port_with("/dev/cu.usbmodem1401", {0x2341, 0x8036})};
    auto sel = ardio::select_port(ports, "", "");
    CHECK(sel.error.empty());
    CHECK(sel.board != nullptr);
    CHECK(sel.board->id == "leonardo");
}
