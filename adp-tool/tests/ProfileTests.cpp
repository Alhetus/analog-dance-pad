#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <vector>

#include "support/PadFixture.h"
#include "support/RecordingBackend.h"

#include <Model/PadDevice.h>
#include <Model/Device.h>

using namespace adp;
using Catch::Approx;

TEST_CASE("LoadProfile applies sensor thresholds, buttons and name", "[profile]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	// v1.3 uses the per-sensor SensorReport path.
	PadDevice pad(rep, "t", makeName("Old"), makeIdent(1, 3, 8, 2), {}, {},
	              {makeSensor(0, 400, 0), makeSensor(1, 400, 1)});
	raw->sent.clear();

	// SendName's SendAndGet reads the device's echo back into the report before
	// applying it, so the echo must carry the accepted name. (The SensorReport
	// sends use Send() alone and need no echo.)
	NameReport echo = makeName("NewName");
	raw->QueueGet(echo);

	// Thresholds are chosen to survive device quantization exactly
	// (0.5*850=425, 0.4*850=340), so the round-trip is lossless.
	json j;
	j["sensors"] = json::array();
	json s0;
	s0["threshold"] = 0.5;
	s0["button"] = 3;
	json s1;
	s1["threshold"] = 0.4;
	s1["button"] = 0;
	j["sensors"].push_back(s0);
	j["sensors"].push_back(s1);
	j["name"] = "NewName";

	pad.LoadProfile(j, DGP_ALL);

	CHECK(pad.Sensor(0)->threshold == Approx(0.5));
	CHECK(pad.Sensor(0)->button == 3);
	CHECK(pad.Sensor(1)->threshold == Approx(0.4));
	CHECK(pad.Sensor(1)->button == 0);
	CHECK(pad.State().name == "NewName");
}

TEST_CASE("LoadProfile honors group flags", "[profile]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1), {}, {},
	              {makeSensor(0, 425, 0)}); // initial threshold 425/850 = 0.5
	raw->sent.clear();

	json j;
	j["sensors"] = json::array();
	json s0;
	s0["threshold"] = 0.9;
	s0["button"] = 5;
	j["sensors"].push_back(s0);

	// DPG_MAPPING only: the button is applied, the threshold is left alone.
	pad.LoadProfile(j, DPG_MAPPING);

	CHECK(pad.Sensor(0)->threshold == Approx(0.5)); // unchanged
	CHECK(pad.Sensor(0)->button == 5);              // applied
}

TEST_CASE("SaveProfile emits sensors, name and version", "[profile]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("MyPad"), makeIdent(1, 3, 8, 2), {}, {},
	              {makeSensor(0, 425, 0), makeSensor(1, 212, 1)});

	json j;
	pad.SaveProfile(j, (DeviceProfileGroups)(DPG_SENSITIVITY | DPG_MAPPING | DPG_DEVICE));

	CHECK(j["name"] == "MyPad");
	CHECK(j.contains("adpToolVersion"));
	REQUIRE(j["sensors"].is_array());
	REQUIRE(j["sensors"].size() == 2);
	CHECK(j["sensors"][0]["threshold"].get<double>() == Approx(0.5)); // 425/850
	CHECK(j["sensors"][0]["button"] == 1);                            // buttonMapping 0 -> 1-based button 1
	CHECK(j["sensors"][1]["button"] == 2);
}

TEST_CASE("SaveProfile honors group flags", "[profile]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("MyPad"), makeIdent(1, 3, 8, 2), {}, {},
	              {makeSensor(0, 425, 0), makeSensor(1, 212, 1)});

	json j;
	pad.SaveProfile(j, DPG_DEVICE); // name only

	CHECK(j.contains("name"));
	CHECK_FALSE(j.contains("sensors"));
	CHECK_FALSE(j.contains("releaseThreshold"));
}
