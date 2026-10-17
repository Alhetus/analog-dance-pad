#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <nlohmann/json.hpp>

#include <Model/Profiles.h>

using namespace adp;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
// A throwaway directory for one test; wiped on construction and destruction.
struct TempDir
{
	fs::path path;
	explicit TempDir(const char* name) : path(fs::temp_directory_path() / name)
	{
		std::error_code ec;
		fs::remove_all(path, ec);
		fs::create_directories(path, ec);
	}
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
	std::string str() const { return path.string(); }
};
} // namespace

TEST_CASE("ValidId accepts safe stems and rejects path tricks", "[profiles]")
{
	CHECK(Profiles::ValidId("my-profile-2"));
	CHECK(Profiles::ValidId("abc123"));

	CHECK_FALSE(Profiles::ValidId(""));
	CHECK_FALSE(Profiles::ValidId(".."));
	CHECK_FALSE(Profiles::ValidId("../etc"));
	CHECK_FALSE(Profiles::ValidId("a/b"));
	CHECK_FALSE(Profiles::ValidId("a\\b"));
	CHECK_FALSE(Profiles::ValidId("a.json")); // '.' not allowed
	CHECK_FALSE(Profiles::ValidId("Upper"));  // must be lowercased first
	CHECK_FALSE(Profiles::ValidId("with space"));
}

TEST_CASE("SlugifyId produces a valid, non-empty id", "[profiles]")
{
	CHECK(Profiles::SlugifyId("My Cool Pad!") == "my-cool-pad");
	CHECK(Profiles::SlugifyId("  ---  ") == "profile"); // no usable chars -> fallback
	CHECK(Profiles::ValidId(Profiles::SlugifyId("Tournament (2026)")));
}

TEST_CASE("Write/Read/List/Delete round-trip", "[profiles]")
{
	TempDir dir("adp-profiles-roundtrip");

	json p;
	p["id"] = "one";
	p["name"] = "One";
	p["sensorCount"] = 4;

	REQUIRE(Profiles::Write(dir.str(), "one", p));
	CHECK(Profiles::Exists(dir.str(), "one"));

	json back;
	REQUIRE(Profiles::Read(dir.str(), "one", back));
	CHECK(back["name"] == "One");
	CHECK(back["sensorCount"] == 4);

	json p2 = p;
	p2["id"] = "two";
	REQUIRE(Profiles::Write(dir.str(), "two", p2));

	json list = Profiles::List(dir.str());
	CHECK(list.is_array());
	CHECK(list.size() == 2);

	REQUIRE(Profiles::Delete(dir.str(), "one"));
	CHECK_FALSE(Profiles::Exists(dir.str(), "one"));
	CHECK(Profiles::List(dir.str()).size() == 1);
}

TEST_CASE("Invalid ids never touch the filesystem", "[profiles]")
{
	TempDir dir("adp-profiles-invalid");

	json p;
	p["id"] = "x";
	CHECK_FALSE(Profiles::Write(dir.str(), "../escape", p));
	CHECK_FALSE(Profiles::Write(dir.str(), "a/b", p));

	json out;
	CHECK_FALSE(Profiles::Read(dir.str(), "../escape", out));
	CHECK_FALSE(Profiles::Delete(dir.str(), "a/b"));

	// Nothing was created.
	CHECK(Profiles::List(dir.str()).empty());
}

TEST_CASE("A profile's id is its filename, whatever the file says", "[profiles]")
{
	// Overwrite/rename used the id stored in the file; a copied or hand-edited
	// file with a stale (or no) id then wrote somewhere else, or nowhere.
	TempDir dir("adp-profiles-id");
	REQUIRE(Profiles::Write(dir.path, "real", json{{"id", "other"}, {"name", "A"}}));
	REQUIRE(Profiles::Write(dir.path, "noid", json{{"name", "B"}}));

	json p;
	REQUIRE(Profiles::ReadProfile(dir.path, "real", p));
	CHECK(p["id"] == "real");
	REQUIRE(Profiles::ReadProfile(dir.path, "noid", p));
	CHECK(p["id"] == "noid");

	for (const auto& listed : Profiles::List(dir.path))
		CHECK((listed["id"] == "real" || listed["id"] == "noid"));
	CHECK(Profiles::List(dir.path).size() == 2);
}

TEST_CASE("ValidUtf8 keeps UTF-8 and replaces anything else", "[profiles]")
{
	const std::string bad = "\xEF\xBF\xBD";
	CHECK(Profiles::ValidUtf8("") == "");
	CHECK(Profiles::ValidUtf8("Cab1 P1") == "Cab1 P1");
	CHECK(Profiles::ValidUtf8("\xC3\x85sa \xF0\x9F\x95\xBA") == "\xC3\x85sa \xF0\x9F\x95\xBA"); // Åsa 🕺
	CHECK(Profiles::ValidUtf8("\xC5sa") == bad + "sa");                                         // Latin-1 Å
	CHECK(Profiles::ValidUtf8("a\xC3") == "a" + bad);                                           // truncated
	CHECK(Profiles::ValidUtf8("\xC0\xAF") == bad + bad);                                        // overlong '/'
	CHECK(Profiles::ValidUtf8("\xED\xA0\x80") == bad + bad + bad);                              // surrogate
	CHECK(Profiles::ValidUtf8("\xF4\x90\x80\x80") == bad + bad + bad + bad);                    // past U+10FFFF
	CHECK_NOTHROW(json(Profiles::ValidUtf8("\xFF\xFE")).dump());
}
