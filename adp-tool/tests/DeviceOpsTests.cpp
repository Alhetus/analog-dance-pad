#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>

#include "support/PadFixture.h"
#include "support/RecordingBackend.h"

#include <Model/Device.h>
#include <Model/PadDevice.h>
#include <Model/Players.h>
#include <Model/Profiles.h>

using namespace adp;
namespace fs = std::filesystem;

// Drives Device::HandleClientMessage end to end: real pads on fake backends in
// the device map, a known player list, and a throwaway profiles dir.
namespace
{
struct Server
{
	fs::path dir = fs::temp_directory_path() / "adp-device-ops";

	Server()
	{
		std::error_code ec;
		fs::remove_all(dir, ec);
		Device::ResetForTest();
		Device::SetProfilesDir(dir);
		Players::PlayerList list;
		list.players = {{"Mike", "00000001", {}, {}}};
		Players::SetForTest(list);
		Players::TakeRenames();
	}

	~Server()
	{
		Device::ResetForTest();
		Players::SetForTest({});
		std::error_code ec;
		fs::remove_all(dir, ec);
	}

	// Adds a pad named `name` at HID path `path`; `raw` is its fake backend.
	RecordingBackend* AddPad(const std::string& path, const char* name, int sensors = 2,
	                         int maxSensorValue = TEST_MAX_SENSOR_VALUE)
	{
		RecordingBackend* raw = nullptr;
		auto rep = makeReporter(raw);
		std::vector<SensorReport> reports;
		for (int i = 0; i < sensors; ++i)
			reports.push_back(makeSensor(i, 425, i));
		Device::AddPadForTest(std::make_unique<PadDevice>(
		    rep, path.c_str(), makeName(name), makeIdent(1, 3, 8, sensors, 0, maxSensorValue),
		    std::vector<LightRuleReport>{}, std::vector<LedMappingReport>{}, reports));
		raw->sent.clear();
		return raw;
	}

	// The one op result (msgType 8) among the replies.
	static json Result(const std::vector<ClientReply>& replies)
	{
		for (const auto& r : replies)
			if (r.message.value("msgType", 0) == 8)
				return r.message;
		FAIL_CHECK("no op result");
		return {};
	}

	static json Send(const json& message) { return Result(Device::HandleClientMessage(message.dump())); }

	static json PadState(const std::string& path)
	{
		json j;
		Device::PadStatesToJson(j);
		return j["pads"][path];
	}
};
} // namespace

TEST_CASE("Profile ops save, list, load, rename and delete a profile", "[deviceops]")
{
	Server server;
	server.AddPad("p1", "Cab1 P1");

	json r = Server::Send({{"pad", "p1"}, {"saveProfile", {{"player", "Mike"}, {"name", "Tech Stamina"}}}});
	REQUIRE(r["ok"] == true);
	CHECK(r["op"] == "saveProfile");
	CHECK(r["name"] == "Tech Stamina");
	const auto dir = Profiles::PlayerPadDir(server.dir, "Mike", "Cab1 P1");
	CHECK(Profiles::Exists(dir, "tech-stamina"));
	CHECK(Server::PadState("p1")["profileId"] == "tech-stamina"); // the pad holds it now

	const auto list = Device::HandleClientMessage(json{{"listProfiles", {{"player", "Mike"}}}}.dump());
	REQUIRE(list.size() == 1);
	CHECK(list[0].message["pads"]["Cab1 P1"].size() == 1);

	r = Server::Send({{"pad", "p1"}, {"loadProfile", {{"player", "Mike"}, {"id", "tech-stamina"}}}});
	CHECK(r["ok"] == true);

	r = Server::Send(
	    {{"pad", "p1"}, {"updateProfile", {{"player", "Mike"}, {"id", "tech-stamina"}, {"name", "Tech"}}}});
	CHECK(r["ok"] == true);
	CHECK(Server::PadState("p1")["profileName"] == "Tech"); // the selection follows

	r = Server::Send({{"pad", "p1"}, {"deleteProfile", {{"player", "Mike"}, {"id", "tech-stamina"}}}});
	CHECK(r["ok"] == true);
	CHECK_FALSE(Profiles::Exists(dir, "tech-stamina"));
	CHECK(Server::PadState("p1")["profileId"] == "");
}

TEST_CASE("Profile ops are refused for unknown players and over-long names", "[deviceops]")
{
	Server server;
	server.AddPad("p1", "Cab1 P1");

	json r = Server::Send({{"pad", "p1"}, {"saveProfile", {{"player", "../x"}, {"name", "a"}}}});
	CHECK(r["ok"] == false);
	CHECK(r["error"] == "unknown player");

	r = Server::Send({{"pad", "p1"}, {"saveProfile", {{"player", "Mike"}, {"name", std::string(65, 'n')}}}});
	CHECK(r["ok"] == false);
	r = Server::Send(
	    {{"pad", "p1"}, {"saveProfile", {{"player", "Mike"}, {"name", "a"}, {"description", std::string(1001, 'd')}}}});
	CHECK(r["ok"] == false);
	CHECK_FALSE(fs::exists(Profiles::PlayerPadDir(server.dir, "Mike", "Cab1 P1")));
}

TEST_CASE("Profile ops are refused while two pads share a name", "[deviceops]")
{
	Server server;
	server.AddPad("p1", "Default");
	server.AddPad("p2", "Default");

	const json r = Server::Send({{"pad", "p1"}, {"saveProfile", {{"player", "Mike"}, {"name", "a"}}}});
	CHECK(r["ok"] == false);
	CHECK(r["error"].get<std::string>().find("also named 'Default'") != std::string::npos);
}

TEST_CASE("A full pad refuses another profile", "[deviceops]")
{
	Server server;
	server.AddPad("p1", "Cab1 P1");
	for (size_t i = 0; i < Profiles::MAX_PER_PAD; ++i)
		REQUIRE(Server::Send({{"pad", "p1"}, {"saveProfile", {{"player", "Mike"}, {"name", "a"}}}})["ok"] == true);
	CHECK(Server::Send({{"pad", "p1"}, {"saveProfile", {{"player", "Mike"}, {"name", "a"}}}})["ok"] == false);
}

TEST_CASE("importProfile stores a sanitized copy", "[deviceops]")
{
	Server server;
	server.AddPad("p1", "Cab1 P1");

	const json profile = {{"sensorCount", 2},
	                      {"sensors", json::array({{{"threshold", 5}, {"junk", "x"}}, json::object()})},
	                      {"lightRules", json::array({{{"onColor", "#FF00AA"}}})}};
	const json r =
	    Server::Send({{"pad", "p1"}, {"importProfile", {{"player", "Mike"}, {"name", "Copy"}, {"profile", profile}}}});
	REQUIRE(r["ok"] == true);
	json stored;
	REQUIRE(Profiles::ReadProfile(Profiles::PlayerPadDir(server.dir, "Mike", "Cab1 P1"), "copy", stored));
	CHECK(stored["sensors"][0] == json{{"threshold", 1.0}});
	CHECK(stored["lightRules"][0]["onColor"] == "#ff00aa");

	const json wrongCount = {{"sensorCount", 3},
	                         {"sensors", json::array({json::object(), json::object(), json::object()})}};
	CHECK(Server::Send(
	          {{"pad", "p1"}, {"importProfile", {{"player", "Mike"}, {"name", "X"}, {"profile", wrongCount}}}})["ok"] ==
	      false);
}

TEST_CASE("Renaming a pad moves its profiles and selection along", "[deviceops]")
{
	Server server;
	RecordingBackend* raw = server.AddPad("p1", "Old");
	server.AddPad("p2", "Taken");
	REQUIRE(Server::Send({{"pad", "p1"}, {"saveProfile", {{"player", "Mike"}, {"name", "a"}}}})["ok"] == true);

	CHECK(Server::Send({{"pad", "p1"}, {"name", "Taken"}})["ok"] == false);

	raw->QueueGet(makeName("New")); // the pad's echo of the accepted name
	const json r = Server::Send({{"pad", "p1"}, {"name", "New"}});
	REQUIRE(r["ok"] == true);
	CHECK(r["name"] == "New");
	CHECK(Profiles::Exists(Profiles::PlayerPadDir(server.dir, "Mike", "New"), "a"));
	CHECK_FALSE(fs::exists(Profiles::PlayerPadDir(server.dir, "Mike", "Old")));
	CHECK(Server::PadState("p1")["profileId"] == "a");
}

TEST_CASE("A renamed player is followed in the pad selections", "[deviceops]")
{
	Server server;
	server.AddPad("p1", "Cab1 P1");
	REQUIRE(Server::Send({{"pad", "p1"}, {"saveProfile", {{"player", "Mike"}, {"name", "a"}}}})["ok"] == true);

	Players::PlayerList list;
	list.players = {{"Mikey", "00000001", {}, {}}};
	list.renamed = {{"Mike", "Mikey", "00000001"}};
	Players::SetForTest(list);
	Device::Update(); // polls the pads, then follows the renames

	const json st = Server::PadState("p1");
	CHECK(st["player"] == "Mikey");
	CHECK(st["recent"] == json::array({"Mikey"}));
}

TEST_CASE("Device ops answer their sender", "[deviceops]")
{
	Server server;
	server.AddPad("p1", "Cab1 P1");

	json r = Server::Send({{"pad", "p1"}, {"releaseMode", 0}});
	CHECK(r["op"] == "setReleaseMode");
	CHECK(r["ok"] == true);
	CHECK(Server::Send({{"pad", "p1"}, {"releaseMode", 1}})["ok"] == false); // global was removed

	r = Server::Send({{"pad", "p1"}, {"sensors", json::array({json::object()})}});
	CHECK(r["op"] == "configure");
	CHECK(r["ok"] == false); // wrong sensor count

	// A successful configure is not answered (slider rate).
	CHECK(Device::HandleClientMessage(
	          json{{"pad", "p1"}, {"sensors", json::array({{{"threshold", 0.3}}, json::object()})}}.dump())
	          .empty());

	// calibrateSensor was removed: no firmware implements it.
	CHECK(Device::HandleClientMessage(R"({"pad":"p1","calibrateSensor":0})").empty());

	// An op on a pad that isn't connected is still answered.
	r = Server::Send({{"pad", "gone"}, {"releaseMode", 0}});
	CHECK(r["error"] == "pad not connected");
	CHECK(r["pad"] == "gone");
}

TEST_CASE("Snapshots say when edits aren't saved to the pad yet", "[deviceops]")
{
	Server server;
	server.AddPad("p1", "Cab1 P1");
	REQUIRE(Server::Send({{"pad", "p1"}, {"releaseMode", 0}})["ok"] == true);

	Device::PublishSnapshot();
	const auto snapshots = Device::GetSnapshots();
	REQUIRE(snapshots->size() == 1);
	json j;
	Device::SnapshotToJson((*snapshots)[0], j);
	CHECK(j["unsaved"] == true);
	CHECK(j["saveFailed"] == false);
}

TEST_CASE("A pad dropped after a failed read saves its unsaved edits first", "[deviceops]")
{
	Server server;
	// Built here so the backend outlives the dropped pad.
	RecordingBackend* raw = nullptr;
	const auto rep = makeReporter(raw);
	Device::AddPadForTest(std::make_unique<PadDevice>(rep, "p1", makeName("Cab1 P1"), makeIdent(1, 3, 8, 1),
	                                                  std::vector<LightRuleReport>{}, std::vector<LedMappingReport>{},
	                                                  std::vector<SensorReport>{makeSensor(0, 425, 0)}));
	REQUIRE(Server::Send({{"pad", "p1"}, {"releaseMode", 0}})["ok"] == true); // now unsaved
	raw->sent.clear();
	raw->QueueReadCount(-1); // the next poll's HID read fails

	Device::Update();

	REQUIRE_FALSE(raw->sent.empty());
	CHECK(raw->sent.back()[0] == REPORT_SAVE_CONFIGURATION);
	CHECK(Server::Send({{"pad", "p1"}, {"releaseMode", 0}})["error"] == "pad not connected");
}

TEST_CASE("Queued sensor edits of one pad fold into one", "[deviceops]")
{
	json a = {{"pad", "p1"}, {"sensors", {{{"threshold", 0.5}}, json::object()}}};
	const json b = {
	    {"pad", "p1"}, {"sensors", {{{"releaseThreshold", 0.2}}, {{"threshold", 0.3}}}}, {"lightRules", json::array()}};
	REQUIRE(Device::IsConfigure(a));
	REQUIRE(Device::IsConfigure(b));
	REQUIRE(Device::MergeConfigure(a, b));
	CHECK(a["sensors"][0] == json{{"threshold", 0.5}, {"releaseThreshold", 0.2}});
	CHECK(a["sensors"][1] == json{{"threshold", 0.3}});
	CHECK(a["lightRules"] == json::array());

	// Arrays of different lengths can't be folded; neither is changed.
	json c = {{"pad", "p1"}, {"sensors", {json::object()}}};
	CHECK_FALSE(Device::MergeConfigure(c, b));
	CHECK(c["sensors"].size() == 1);

	CHECK_FALSE(Device::IsConfigure({{"sensors", json::array()}}));                    // no pad
	CHECK_FALSE(Device::IsConfigure({{"pad", "p1"}, {"releaseMode", 0}}));             // another op
	CHECK_FALSE(Device::IsConfigure(json::parse("not json", nullptr, false)));         // invalid
	CHECK_FALSE(Device::IsConfigure({{"pad", "p1"}, {"name", "x"}, {"sensors", {}}})); // a rename
}

TEST_CASE("A refused op is answered, anything else is not", "[deviceops]")
{
	const auto replies = Device::Refuse({{"pad", "p1"}, {"loadProfile", json::object()}}, "busy");
	REQUIRE(replies.size() == 1);
	CHECK(replies[0].message["op"] == "loadProfile");
	CHECK(replies[0].message["pad"] == "p1");
	CHECK(replies[0].message["ok"] == false);
	CHECK(replies[0].message["error"] == "busy");
	CHECK(Device::Refuse({{"rescanPlayers", true}}, "busy").empty());
	CHECK(Device::Refuse(json::parse("{", nullptr, false), "busy").empty());
}

TEST_CASE("A message asking for two ops is refused, not half applied", "[deviceops]")
{
	Server server;
	RecordingBackend* raw = server.AddPad("p1", "Cab1 P1");
	// Used to become a rename that silently dropped the sensors.
	const json r = Server::Send(
	    {{"pad", "p1"}, {"name", "Other"}, {"sensors", json::array({{{"threshold", 0.9}}, json::object()})}});
	CHECK(r["ok"] == false);
	CHECK(raw->sent.empty());
}

TEST_CASE("Op results echo the client's requestId", "[deviceops]")
{
	Server server;
	server.AddPad("p1", "Cab1 P1");
	CHECK(Server::Send({{"pad", "p1"}, {"releaseMode", 0}, {"requestId", 7}})["requestId"] == 7);
	CHECK_FALSE(Server::Send({{"pad", "p1"}, {"releaseMode", 0}}).contains("requestId"));
	CHECK(Device::Refuse({{"pad", "p1"}, {"releaseMode", 0}, {"requestId", "a"}}, "busy")[0].message["requestId"] ==
	      "a");
}

TEST_CASE("The device list carries each pad's firmware and sensor scale", "[deviceops]")
{
	Server server;
	server.AddPad("p1", "Cab1 P1", 2, 1024);
	json list;
	Device::DeviceListToJson(list);
	CHECK(list["devices"][0]["maxSensorValue"] == 1024);
	CHECK(list["devices"][0]["firmware"] == "1.3");
}
