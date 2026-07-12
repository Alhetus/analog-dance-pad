#include <catch2/catch_test_macros.hpp>

#include <Model/Device.h>

using namespace adp;

TEST_CASE("SnapshotToJson emits the WebSocket contract fields", "[snapshot]")
{
	SensorSnapshot snap;
	snap.connected = true;
	snap.deviceCount = 2;
	snap.name = "MyPad";
	snap.pollingRate = 1000;
	snap.releaseThreshold = 0.8;
	snap.releaseMode = 1;

	SensorState s;
	s.threshold = 0.5;
	s.releaseThreshold = 0.4;
	s.value = 0.3;
	s.resistorValue = 10;
	s.button = 4;
	s.pressed = true;
	snap.sensors.push_back(s);

	json j;
	Device::SnapshotToJson(snap, j);

	CHECK(j["msgType"] == 1);
	// The field is named deviceIndex but is sourced from deviceCount (pinned quirk).
	CHECK(j["deviceIndex"] == 2);
	CHECK(j["name"] == "MyPad");
	CHECK(j["pollingRate"] == 1000);
	CHECK(j["releaseThreshold"] == 0.8);
	CHECK(j["releaseMode"] == 1);

	REQUIRE(j["sensors"].is_array());
	REQUIRE(j["sensors"].size() == 1);

	const json& sj = j["sensors"][0];
	CHECK(sj["threshold"] == 0.5);
	CHECK(sj["releaseThreshold"] == 0.4);
	CHECK(sj["value"] == 0.3);
	CHECK(sj["resistorValue"] == 10);
	CHECK(sj["button"] == 4);
	CHECK(sj["pressed"] == true);
}

TEST_CASE("SnapshotToJson emits an empty array when there are no sensors", "[snapshot]")
{
	SensorSnapshot snap;
	json j;
	Device::SnapshotToJson(snap, j);

	REQUIRE(j["sensors"].is_array());
	CHECK(j["sensors"].empty());
}
