#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <thread>

#include <Model/Players.h>
#include <Model/Profiles.h>
#include <Model/Reporter.h>

using namespace adp;
namespace fs = std::filesystem;
using json = nlohmann::json;

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
};

void WriteFile(const fs::path& p, const std::string& text)
{
	fs::create_directories(p.parent_path());
	std::ofstream(p, std::ios::binary) << text;
}

const std::string kAsa = "\xC3\x85sa"; // "Åsa" in UTF-8
} // namespace

TEST_CASE("ParseDisplayName reads Editable.ini", "[players]")
{
	CHECK(Players::ParseDisplayName("[Editable]\r\nDisplayName=Mike\r\nWeightPounds=0\r\n") == "Mike");
	CHECK(Players::ParseDisplayName("\xEF\xBB\xBF[Editable]\nDisplayName= " + kAsa + " \n") == kAsa);
	CHECK(Players::ParseDisplayName("[Editable]\nDisplayName=\n") == "");
	CHECK(Players::ParseDisplayName("[Editable]\nLastUsedHighScoreName=X\n") == "");
	// A Latin-1 file edited by hand: still a name json can serialize.
	CHECK(Players::ParseDisplayName("DisplayName=\xC5sa\n") == "\xEF\xBF\xBDsa");
}

TEST_CASE("AvatarMime matches avatar.<ext> in any case", "[players]")
{
	CHECK(Players::AvatarMime("avatar.png") == "image/png");
	CHECK(Players::AvatarMime("Avatar.JPG") == "image/jpeg");
	CHECK(Players::AvatarMime("AVatar.mp4") == "video/mp4");
	CHECK(Players::AvatarMime("avatar.txt") == "");
	CHECK(Players::AvatarMime("myavatar.png") == "");
	CHECK(Players::AvatarMime("avatar") == "");
}

TEST_CASE("PlanRenames follows a renamed folder unless the old name is still used", "[players]")
{
	auto r = Players::PlanRenames({{"00000001", "Mike"}}, {{"00000001", "Mikey"}});
	REQUIRE(r.size() == 1);
	CHECK(r[0].from == "Mike");
	CHECK(r[0].to == "Mikey");

	// Another "Mike" still exists: the shared dir stays put.
	CHECK(Players::PlanRenames({{"00000001", "Mike"}, {"00000002", "Mike"}},
	                           {{"00000001", "Mikey"}, {"00000002", "Mike"}})
	          .empty());
	// New folder, unchanged name, first run: nothing to do.
	CHECK(Players::PlanRenames({}, {{"00000001", "Mike"}}).empty());
	CHECK(Players::PlanRenames({{"00000001", "Mike"}}, {{"00000001", "Mike"}}).empty());
}

TEST_CASE("Base64 encodes with padding", "[players]")
{
	CHECK(Players::Base64("") == "");
	CHECK(Players::Base64("f") == "Zg==");
	CHECK(Players::Base64("fo") == "Zm8=");
	CHECK(Players::Base64("foo") == "Zm9v");
	CHECK(Players::Base64("foobar") == "Zm9vYmFy");
}

TEST_CASE("EscapeName yields one safe, reversible path component", "[profiles]")
{
	for (const std::string& name :
	     {std::string("Mike"), kAsa, std::string("a/b\\c:d"), std::string(".."), std::string("CON"), std::string("x."),
	      std::string("100% "), std::string("Cab1 P1")})
	{
		const std::string esc = Profiles::EscapeName(name);
		CHECK(Profiles::UnescapeName(esc) == name);
		CHECK(esc.find_first_of("/\\:<>\"|?*") == std::string::npos);
		CHECK(esc != "..");
	}
	CHECK(Profiles::EscapeName("Mike") == "Mike");
	CHECK(Profiles::EscapeName(kAsa) == kAsa); // stays readable on disk
	CHECK(Profiles::EscapeName("CON") != "CON");
}

TEST_CASE("Scan reads players, avatars, counts and follows renames", "[players]")
{
	TempDir local("adp-players-local");
	TempDir store("adp-players-store");

	WriteFile(local.path / "00000000" / "Editable.ini", "[Editable]\nDisplayName=" + kAsa + "\n");
	WriteFile(local.path / "00000000" / "AVATAR.PNG", "png");
	WriteFile(local.path / "00000001" / "Editable.ini", "[Editable]\nDisplayName=Mike\n");
	WriteFile(local.path / "00000002" / "editable.INI", "[Editable]\nDisplayName=Mike\n"); // duplicate, odd case
	WriteFile(local.path / "00000003" / "Editable.ini", "[Editable]\nDisplayName=\n");     // skipped

	const auto mikePad = Profiles::PlayerPadDir(store.path, "Mike", "Cab1 P1");
	REQUIRE(Profiles::Write(mikePad, "stamina", {{"id", "stamina"}}));
	REQUIRE(Profiles::Write(mikePad, "tech", {{"id", "tech"}}));

	auto list = Players::Scan(local.path, store.path);
	REQUIRE(list.players.size() == 2);
	CHECK(list.players[0].name == "Mike");
	CHECK(list.players[0].profileCounts.at("Cab1 P1") == 2);
	CHECK(list.players[1].name == kAsa);
	CHECK(list.players[1].avatar.filename() == "AVATAR.PNG");

	// Both Mike folders rename: the profiles follow to the new name.
	WriteFile(local.path / "00000001" / "Editable.ini", "[Editable]\nDisplayName=Mikey\n");
	WriteFile(local.path / "00000002" / "Editable.ini", "[Editable]\nDisplayName=Mikey\n");
	list = Players::Scan(local.path, store.path);
	CHECK(fs::exists(Profiles::PlayerPadDir(store.path, "Mikey", "Cab1 P1") / "tech.json"));
	CHECK_FALSE(fs::exists(mikePad));
}

TEST_CASE("Scan keeps the last known players while LocalProfiles is unmounted", "[players]")
{
	TempDir local("adp-players-unmount-local");
	TempDir store("adp-players-unmount-store");
	WriteFile(local.path / "00000001" / "Editable.ini", "[Editable]\nDisplayName=Mike\n");
	REQUIRE(Profiles::Write(Profiles::PlayerPadDir(store.path, "Mike", "P1"), "a", {{"id", "a"}}));

	const auto good = Players::Scan(local.path, store.path);
	REQUIRE(good.localOk);
	REQUIRE(good.players.size() == 1);

	// LocalProfiles gone: players are kept, flagged stale.
	auto down = Players::Scan(local.path / "missing", store.path, good);
	CHECK_FALSE(down.localOk);
	REQUIRE(down.players.size() == 1);
	CHECK(down.players[0].name == "Mike");
	CHECK(Players::ToJson(down)["localProfiles"] == false);

	// Unmounted Linux mount point: the dir exists but is empty. Same result.
	TempDir emptyMount("adp-players-unmount-empty");
	down = Players::Scan(emptyMount.path, store.path, good);
	CHECK_FALSE(down.localOk);
	CHECK(down.players.size() == 1);

	// Mounted again: everything fresh.
	const auto back = Players::Scan(local.path, store.path, down);
	CHECK(back.localOk);
	CHECK(back.players[0].profileCounts.at("P1") == 1);
}

TEST_CASE("Recount refreshes profile counts without reading LocalProfiles", "[players]")
{
	TempDir store("adp-players-recount-store");
	Players::PlayerList before;
	before.localOk = false; // carried over untouched
	before.players.push_back({"Mike", "00000001", {}, {}});
	REQUIRE(Profiles::Write(Profiles::PlayerPadDir(store.path, "Mike", "P1"), "a", {{"id", "a"}}));

	std::unique_lock<std::mutex> hold;
	const auto after = Players::Recount(store.path, before, &hold);
	CHECK(hold.owns_lock()); // handed over, so the caller publishes before any op runs
	hold.unlock();
	CHECK_FALSE(after.localOk);
	REQUIRE(after.players.size() == 1);
	CHECK(after.players[0].profileCounts.at("P1") == 1);
}

TEST_CASE("Sanitize keeps only the storable settings of an imported profile", "[profiles]")
{
	const json in = {{"id", "../evil"},         {"name", "x"},
	                 {"sensorCount", 2},        {"sensors", json::array({json::object(), json::object()})},
	                 {"releaseThreshold", 0.5}, {"lightRules", json::array()},
	                 {"description", 42},       {"junk", "y"}};
	const json out = Profiles::Sanitize(in);
	CHECK(out["sensorCount"] == 2);
	CHECK_FALSE(out.contains("releaseThreshold")); // the removed global release ratio
	CHECK(out["description"] == "");               // wrong type replaced
	CHECK_FALSE(out.contains("id"));               // set by the server, never taken from the client
	CHECK_FALSE(out.contains("junk"));
	CHECK_FALSE(out.contains("ledMappings"));

	CHECK(Profiles::Sanitize({{"sensors", json::array()}}).is_null()); // no sensorCount
	CHECK(Profiles::Sanitize({{"sensorCount", 4}, {"sensors", "x"}}).is_null());
	CHECK(Profiles::Sanitize("not an object").is_null());
	// A sensors array that doesn't match sensorCount would load only partly.
	CHECK(Profiles::Sanitize({{"sensorCount", 4}, {"sensors", json::array({json::object()})}}).is_null());
	// More light slots than a pad has.
	CHECK(Profiles::Sanitize({{"sensorCount", 0},
	                          {"sensors", json::array()},
	                          {"lightRules", json(std::vector<json>(MAX_LIGHT_RULES + 1, json::object()))}})
	          .is_null());
}

TEST_CASE("Profiles store under a UTF-8 player dir", "[profiles]")
{
	TempDir store("adp-players-utf8");
	const auto dir = Profiles::PlayerPadDir(store.path, kAsa, "Cab2 P2");
	REQUIRE(Profiles::Write(dir, "a", {{"id", "a"}}));
	CHECK(Profiles::List(dir).size() == 1);
	CHECK(Profiles::PathU8(dir.parent_path().filename()) == kAsa);
}

TEST_CASE("Recounts don't hold back the next full scan", "[players]")
{
	TempDir local("adp-players-starve-local"), store("adp-players-starve-store");
	WriteFile(local.path / "00000001" / "Editable.ini", "DisplayName=Mike\n");

	const auto waitFor = [](const char* name, bool recount) {
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < until && !Players::Has(name))
		{
			if (recount)
				Players::RequestRecount(); // a profile op, faster than the scan interval
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		return Players::Has(name);
	};

	Players::Start(local.path, store.path, std::chrono::seconds(1));
	CHECK(waitFor("Mike", false));
	WriteFile(local.path / "00000002" / "Editable.ini", "DisplayName=Anna\n");
	CHECK(waitFor("Anna", true));
	Players::Stop();
	Players::SetForTest({});
}
