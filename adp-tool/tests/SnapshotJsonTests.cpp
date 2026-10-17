#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include <Model/Device.h>

using namespace adp;

TEST_CASE("SnapshotToJson emits the WebSocket contract fields", "[snapshot]")
{
	SensorSnapshot snap;
	snap.connected = true;
	snap.path = "hid#1";
	snap.name = "MyPad";
	snap.pollingRate = 1000;
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
	// Clients route every message by the pad's HID path.
	CHECK(j["pad"] == "hid#1");
	CHECK(j["name"] == "MyPad");
	CHECK(j["pollingRate"] == 1000);
	CHECK_FALSE(j.contains("releaseThreshold")); // the removed global release ratio
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
	snap.lights.lightRules[0] =
	    LightRule{true, false, RgbColor(255, 0, 0), RgbColor(0, 0, 0), RgbColor(0, 0, 0), RgbColor(0, 0, 0)};
	snap.lights.ledMappings[0] = LedMapping{0, 1, 2, 3};

	json j;
	Device::LightsToJson(snap, j);

	CHECK(j["msgType"] == 4);
	CHECK(j.contains("pad"));
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

TEST_CASE("SnapshotToJson quantizes values so a resting pad serializes identically", "[snapshot]")
{
	SensorSnapshot snap;
	SensorState s;
	s.threshold = 425.0 / MAX_SENSOR_VALUE;
	s.value = 100.2 / MAX_SENSOR_VALUE; // an average over the tick
	snap.sensors.push_back(s);
	json a;
	Device::SnapshotToJson(snap, a);

	snap.sensors[0].value = 99.9 / MAX_SENSOR_VALUE; // the same device value, rounded
	json b;
	Device::SnapshotToJson(snap, b);

	CHECK(a.dump() == b.dump());
	CHECK(a["sensors"][0]["value"] == 0.1176); // 100/850, 4 decimals
	// 4 decimals still map back to the exact device threshold.
	CHECK(std::lround(a["sensors"][0]["threshold"].get<double>() * MAX_SENSOR_VALUE) == 425);
}
