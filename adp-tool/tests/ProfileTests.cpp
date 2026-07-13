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

TEST_CASE("Gain and button map to separate profile groups", "[profile]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("MyPad"), makeIdent(1, 3, 8, 1), {}, {}, {makeSensor(0, 425, 0)});

	// DPG_GAIN emits gain (resistorValue) but never the button mapping.
	json g;
	pad.SaveProfile(g, DPG_GAIN);
	REQUIRE(g["sensors"].is_array());
	CHECK(g["sensors"][0].contains("resistorValue"));
	CHECK_FALSE(g["sensors"][0].contains("button"));

	// DPG_MAPPING emits the button mapping but never the gain.
	json m;
	pad.SaveProfile(m, DPG_MAPPING);
	CHECK(m["sensors"][0].contains("button"));
	CHECK_FALSE(m["sensors"][0].contains("resistorValue"));
}

TEST_CASE("LoadProfile applies gain only under DPG_GAIN", "[profile]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	// features = FEATURE_DIGIPOT (1 << 1) so gain writes are honored.
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1, 1 << 1), {}, {}, {makeSensor(0, 425, 0)});

	json j;
	j["sensors"] = json::array();
	json s0;
	s0["resistorValue"] = 42;
	s0["button"] = 5;
	j["sensors"].push_back(s0);

	// DPG_MAPPING must NOT touch gain (that is DPG_GAIN's job now).
	pad.LoadProfile(j, DPG_MAPPING);
	CHECK(pad.Sensor(0)->resistorValue == 0); // unchanged
	CHECK(pad.Sensor(0)->button == 5);        // applied

	// DPG_GAIN applies gain and leaves the button alone.
	pad.LoadProfile(j, DPG_GAIN);
	CHECK(pad.Sensor(0)->resistorValue == 42);
}

TEST_CASE("LoadProfile restores per-sensor release threshold in individual mode", "[profile]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1), {}, {}, {makeSensor(0, 425, 0)});
	// Per-sensor release is only honored in individual mode; other modes derive it.
	pad.SetReleaseMode(RELEASE_INDIVIDUAL);

	// 0.5*850=425, 0.2*850=170 — both survive device quantization exactly, so a
	// distinct release proves it is no longer clobbered to equal the threshold.
	json j;
	j["sensors"] = json::array();
	json s0;
	s0["threshold"] = 0.5;
	s0["releaseThreshold"] = 0.2;
	j["sensors"].push_back(s0);

	pad.LoadProfile(j, DPG_SENSITIVITY);

	CHECK(pad.Sensor(0)->threshold == Approx(0.5));
	CHECK(pad.Sensor(0)->releaseThreshold == Approx(0.2));
}

TEST_CASE("LoadProfile applies a release-only patch in individual mode", "[profile]")
{
	// Reproduces the reported bug: dragging only the per-sensor release slider
	// sends a patch with releaseThreshold but no threshold. That edit must reach
	// the device instead of being dropped for lacking a threshold field.
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	// Start from threshold 0.5, release 0.25 (212/850 via makeSensor's thr/2).
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1), {}, {}, {makeSensor(0, 425, 0)});
	pad.SetReleaseMode(RELEASE_INDIVIDUAL);

	json j;
	j["sensors"] = json::array();
	json s0;
	s0["releaseThreshold"] = 0.2; // no "threshold" key
	j["sensors"].push_back(s0);

	pad.LoadProfile(j, DPG_SENSITIVITY);

	// Threshold is untouched; the release-only edit is applied.
	CHECK(pad.Sensor(0)->threshold == Approx(0.5));
	CHECK(pad.Sensor(0)->releaseThreshold == Approx(0.2));
}
