#include <catch2/catch_test_macros.hpp>

#include <Model/Firmware.h>

using namespace adp;

TEST_CASE("ParseArchType maps names and aliases", "[firmware]")
{
	CHECK(BoardTypeStruct::ParseArchType("avr") == ARCH_AVR);
	CHECK(BoardTypeStruct::ParseArchType("m32u4") == ARCH_AVR);
	CHECK(BoardTypeStruct::ParseArchType("esp") == ARCH_ESP);
	CHECK(BoardTypeStruct::ParseArchType("esp32s3") == ARCH_ESP);
	CHECK(BoardTypeStruct::ParseArchType("avrard") == ARCH_AVR_ARD);
	CHECK(BoardTypeStruct::ParseArchType("nonsense") == ARCH_UNKNOWN);
}

TEST_CASE("ParseBoardType maps names and aliases", "[firmware]")
{
	CHECK(BoardTypeStruct::ParseBoardType("fsrio1") == BOARD_FSRIO_V1);
	CHECK(BoardTypeStruct::ParseBoardType("fsriov1") == BOARD_FSRIO_V1);
	CHECK(BoardTypeStruct::ParseBoardType("fsrio2") == BOARD_FSRIO_V2);
	CHECK(BoardTypeStruct::ParseBoardType("fsriov2") == BOARD_FSRIO_V2);
	CHECK(BoardTypeStruct::ParseBoardType("fsrio3") == BOARD_FSRIO_V3);
	CHECK(BoardTypeStruct::ParseBoardType("fsriov3") == BOARD_FSRIO_V3);
	CHECK(BoardTypeStruct::ParseBoardType("fsrminipad") == BOARD_FSRMINIPAD);
	CHECK(BoardTypeStruct::ParseBoardType("minipadv4") == BOARD_FSRMINIPAD_V4);
	CHECK(BoardTypeStruct::ParseBoardType("teensy2") == BOARD_TEENSY2);
	CHECK(BoardTypeStruct::ParseBoardType("leonardo") == BOARD_LEONARDO);
	CHECK(BoardTypeStruct::ParseBoardType("nope") == BOARD_UNKNOWN);
}

TEST_CASE("BoardTypeStruct string ctor splits on underscore", "[firmware]")
{
	// One part: arch defaults to AVR, the token is the board.
	BoardTypeStruct one("fsrio1");
	CHECK(one.archType == ARCH_AVR);
	CHECK(one.boardType == BOARD_FSRIO_V1);

	// Two parts: arch_board.
	BoardTypeStruct two("esp_fsriov2");
	CHECK(two.archType == ARCH_ESP);
	CHECK(two.boardType == BOARD_FSRIO_V2);

	// Three or more parts match neither branch, so both stay UNKNOWN.
	BoardTypeStruct three("a_b_c");
	CHECK(three.archType == ARCH_UNKNOWN);
	CHECK(three.boardType == BOARD_UNKNOWN);
}

TEST_CASE("CompatibleWith checks arch and (strict) board", "[firmware]")
{
	BoardTypeStruct avr1(ARCH_AVR, BOARD_FSRIO_V1);
	BoardTypeStruct avr1b(ARCH_AVR, BOARD_FSRIO_V1);
	BoardTypeStruct avr2(ARCH_AVR, BOARD_FSRIO_V2);
	BoardTypeStruct ard1(ARCH_AVR_ARD, BOARD_FSRIO_V1);
	BoardTypeStruct esp1(ARCH_ESP, BOARD_FSRIO_V1);
	BoardTypeStruct avrUnknown(ARCH_AVR, BOARD_UNKNOWN);

	// Strict (default): same arch and same known board.
	CHECK(avr1.CompatibleWith(avr1b));
	CHECK_FALSE(avr1.CompatibleWith(avr2));   // different board
	CHECK_FALSE(avr1.CompatibleWith(esp1));   // different arch

	// The other side's ARCH_AVR_ARD is coerced to ARCH_AVR.
	CHECK(avr1.CompatibleWith(ard1));
	// ponytail: coercion only applies to the argument, so the reverse is
	// asymmetric. Pinned as current behavior.
	CHECK_FALSE(ard1.CompatibleWith(avr1));

	// Strict rejects an UNKNOWN board on either side.
	CHECK_FALSE(avr1.CompatibleWith(avrUnknown));
	CHECK_FALSE(avrUnknown.CompatibleWith(avr1));

	// Non-strict: arch match is enough, board is ignored.
	CHECK(avr1.CompatibleWith(avr2, false));
	CHECK(avr1.CompatibleWith(avrUnknown, false));
	CHECK_FALSE(avr1.CompatibleWith(esp1, false));
}

TEST_CASE("BoardTypeStruct::ToString pins current behavior", "[firmware]")
{
	CHECK(BoardTypeStruct(ARCH_AVR, BOARD_FSRIO_V1).ToString() == "avr_fsrio1");
	CHECK(BoardTypeStruct(ARCH_ESP, BOARD_FSRIO_V2).ToString() == "esp_fsriov2");
	CHECK(BoardTypeStruct(ARCH_AVR, BOARD_FSRIO_V3).ToString() == "avr_fsriov3");
	CHECK(BoardTypeStruct(ARCH_AVR, BOARD_FSRMINIPAD).ToString() == "avr_fsrminipad");
	CHECK(BoardTypeStruct(ARCH_AVR, BOARD_FSRMINIPAD_V4).ToString() == "avr_minipadv4");

	// ponytail: BUG pinned. TEENSY2 / LEONARDO / FSRMINIPAD_V2 have no ToString
	// case, so they render as "unknown"; parse aliases also collapse. So
	// parse->ToString is not a round-trip. Fix separately (see plan).
	CHECK(BoardTypeStruct(ARCH_AVR, BOARD_TEENSY2).ToString() == "avr_unknown");
	CHECK(BoardTypeStruct(ARCH_AVR, BOARD_LEONARDO).ToString() == "avr_unknown");
	CHECK(BoardTypeStruct(ARCH_AVR, BOARD_FSRMINIPAD_V2).ToString() == "avr_unknown");
	CHECK(BoardTypeStruct(ARCH_UNKNOWN, BOARD_UNKNOWN).ToString() == "unknown");
}
