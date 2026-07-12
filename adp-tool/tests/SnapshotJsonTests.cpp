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

	// Lights are NOT in the 60Hz snapshot — they ride their own message (msgType 4).
	CHECK_FALSE(j.contains("lightRules"));
	CHECK_FALSE(j.contains("ledMappings"));
}

TEST_CASE("LightsToJson emits the lights message in profile shape", "[snapshot]")
{
	SensorSnapshot snap;
	snap.lights.lightRules[0] = LightRule{true, false, RgbColor(255, 0, 0), RgbColor(0, 0, 0),
	                                       RgbColor(0, 0, 0), RgbColor(0, 0, 0)};
	snap.lights.ledMappings[0] = LedMapping{0, 1, 2, 3};

	json j;
	Device::LightsToJson(snap, j);

	CHECK(j["msgType"] == 4);
	// Same shape a saved profile uses, so the client can compare field-for-field.
	REQUIRE(j["lightRules"].is_array());
	REQUIRE(j["lightRules"].size() == 1);
	CHECK(j["lightRules"][0]["fadeOn"] == true);
	CHECK(j["lightRules"][0]["onColor"] == "#ff0000");
	REQUIRE(j["ledMappings"].is_array());
	REQUIRE(j["ledMappings"].size() == 1);
	CHECK(j["ledMappings"][0]["sensorIndex"] == 1);
	CHECK(j["ledMappings"][0]["ledIndexEnd"] == 3);
}

TEST_CASE("SnapshotToJson emits an empty array when there are no sensors", "[snapshot]")
{
	SensorSnapshot snap;
	json j;
	Device::SnapshotToJson(snap, j);

	REQUIRE(j["sensors"].is_array());
	CHECK(j["sensors"].empty());
}
