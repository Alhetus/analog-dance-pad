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
	CHECK_FALSE(Profiles::ValidId("a.json"));   // '.' not allowed
	CHECK_FALSE(Profiles::ValidId("Upper"));    // must be lowercased first
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
