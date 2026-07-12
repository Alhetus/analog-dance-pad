#include <catch2/catch_test_macros.hpp>

#include <Model/Device.h>

using namespace adp;

TEST_CASE("RgbColor parses #rrggbb hex", "[color]")
{
	RgbColor c("#ff8000");
	CHECK(c.red == 0xFF);
	CHECK(c.green == 0x80);
	CHECK(c.blue == 0x00);
}

TEST_CASE("RgbColor accepts hex without leading #", "[color]")
{
	RgbColor c("ff8000");
	CHECK(c.red == 0xFF);
	CHECK(c.green == 0x80);
	CHECK(c.blue == 0x00);
}

TEST_CASE("RgbColor falls back to black on malformed input", "[color]")
{
	// Fewer than three hex bytes, or non-hex, leaves all channels 0.
	for (const char* bad : { "", "#", "xyz", "#12", "12", "#gg0000" })
	{
		RgbColor c(bad);
		CHECK(c.red == 0);
		CHECK(c.green == 0);
		CHECK(c.blue == 0);
	}
}

TEST_CASE("RgbColor::ToString formats lowercase #rrggbb", "[color]")
{
	CHECK(RgbColor(0xFF, 0x80, 0x00).ToString() == "#ff8000");
	CHECK(RgbColor(0, 0, 0).ToString() == "#000000");
	CHECK(RgbColor(0x0A, 0x0B, 0x0C).ToString() == "#0a0b0c");
}

TEST_CASE("RgbColor round-trips through ToString", "[color]")
{
	RgbColor original(0x12, 0x34, 0x56);
	RgbColor reparsed(original.ToString());
	CHECK(reparsed.red == original.red);
	CHECK(reparsed.green == original.green);
	CHECK(reparsed.blue == original.blue);
}
