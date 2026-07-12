#include <catch2/catch_test_macros.hpp>

#include <string>

#include <Model/Utils.h>

using namespace adp;

// widen (UTF-8 -> wchar_t) then narrow (wchar_t -> UTF-8) must be the identity.
// This stays portable across 16-bit wchar_t (Windows, surrogate pairs) and
// 32-bit wchar_t (Linux/macOS) because both directions are symmetric.
static std::string roundtrip(const std::string& utf8)
{
	std::wstring w = widen(utf8);
	return narrow(w.c_str(), w.size());
}

TEST_CASE("narrow decodes ASCII wide strings", "[utils]")
{
	CHECK(narrow(L"", 0) == "");
	CHECK(narrow(L"ABC", 3) == "ABC");
}

TEST_CASE("widen encodes ASCII with one wchar per byte", "[utils]")
{
	CHECK(widen("").empty());
	CHECK(widen("ABC").size() == 3);
}

TEST_CASE("narrow/widen round-trips UTF-8", "[utils]")
{
	CHECK(roundtrip("") == "");
	CHECK(roundtrip("hello") == "hello");
	CHECK(roundtrip("\xC3\xA9") == "\xC3\xA9");                 // U+00E9 e-acute (2 bytes)
	CHECK(roundtrip("\xE2\x82\xAC") == "\xE2\x82\xAC");         // U+20AC euro sign (3 bytes)
	CHECK(roundtrip("\xF0\x9F\x98\x80") == "\xF0\x9F\x98\x80"); // U+1F600 (4 bytes, astral)
	CHECK(roundtrip("a\xC3\xA9"
	                "b\xE2\x82\xAC"
	                "c") == "a\xC3\xA9"
	                        "b\xE2\x82\xAC"
	                        "c"); // mixed
}
