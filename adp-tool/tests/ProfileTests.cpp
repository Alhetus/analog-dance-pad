#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <vector>

#include "support/PadFixture.h"
#include "support/RecordingBackend.h"

#include <Model/PadDevice.h>
#include <Model/Device.h>

using namespace adp;
using Catch::Approx;

static constexpr DeviceProfileGroups PROFILE_TEST_GROUPS = DPG_SENSITIVITY | DPG_GAIN;

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

	pad.LoadProfile(j, DPG_ALL);

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

TEST_CASE("LoadProfile ignores sensor entries past the pad's sensor count", "[profile]")
{
	// A partial entry (release only) used to read mySensors[idx] before the
	// index was checked: out of bounds for every entry past the pad's sensors.
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 2), {}, {},
	              {makeSensor(0, 425, 0), makeSensor(1, 425, 1)});
	raw->sent.clear();

	json j;
	j["sensors"] = json::array();
	for (int i = 0; i < 6; ++i)
		j["sensors"].push_back({{"releaseThreshold", 0.2}});

	pad.LoadProfile(j, DPG_SENSITIVITY);

	CHECK(raw->sent.size() == 2); // one SensorReport per real sensor
	CHECK(pad.Sensor(1)->releaseThreshold == Approx(0.2));
}

TEST_CASE("LoadProfile treats null light slots as disabled and skips unchanged ones", "[profile]")
{
	// A profile saved from a pad with gaps between its enabled slots has nulls
	// in the gaps; those used to throw halfway through applying the profile.
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1, IdentificationV2Report::FEATURE_LIGHTS), {}, {},
	              {makeSensor(0, 425, 0)});
	raw->sent.clear();

	json rule = {{"fadeOn", true},        {"fadeOff", false},         {"onColor", "#ff0000"},
	             {"offColor", "#000000"}, {"onFadeColor", "#00ff00"}, {"offFadeColor", "#0000ff"}};
	json j;
	j["lightRules"] = json::array({rule, nullptr, rule});
	j["ledMappings"] =
	    json::array({nullptr, {{"lightRuleIndex", 2}, {"sensorIndex", 0}, {"ledIndexBegin", 0}, {"ledIndexEnd", 4}}});

	CHECK_NOTHROW(pad.LoadProfile(j, DPG_LIGHTS));
	CHECK(pad.Lights().lightRules.size() == 2);
	CHECK(pad.Lights().lightRules.contains(0));
	CHECK(pad.Lights().lightRules.contains(2));
	CHECK(pad.Lights().ledMappings.size() == 1);
	CHECK(pad.Lights().ledMappings.contains(1));
	CHECK(raw->sent.size() == 3); // only the enabled slots; nothing was on to disable

	// Loading the same lights again sends nothing: every slot already matches.
	raw->sent.clear();
	pad.LoadProfile(j, DPG_LIGHTS);
	CHECK(raw->sent.empty());

	// Dropping a rule disables just that slot.
	j["lightRules"] = json::array({rule});
	pad.LoadProfile(j, DPG_LIGHTS);
	CHECK(raw->sent.size() == 1);
	CHECK_FALSE(pad.Lights().lightRules.contains(2));
}

TEST_CASE("LoadProfile refuses malformed lights without sending any", "[profile]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1, IdentificationV2Report::FEATURE_LIGHTS), {}, {},
	              {makeSensor(0, 425, 0)});
	raw->sent.clear();

	const json good = {{"lightRuleIndex", 0}, {"sensorIndex", 0}, {"ledIndexBegin", 0}, {"ledIndexEnd", 4}};
	json bad = good;
	SECTION("an index that doesn't fit the report's byte")
	{
		bad["ledIndexEnd"] = 256; // used to wrap to 0
	}
	SECTION("a field of the wrong type")
	{
		bad["sensorIndex"] = "0"; // used to throw after earlier slots were sent
	}
	SECTION("more slots than the pad has")
	{
		bad = nullptr;
	}
	SECTION("a sensor the pad doesn't have")
	{
		bad["sensorIndex"] = 1;
	}
	SECTION("LEDs past the pad's last")
	{
		bad["ledIndexEnd"] = 17;
	}
	SECTION("a reversed LED range")
	{
		bad["ledIndexBegin"] = 5;
	}
	SECTION("a rule slot past the last")
	{
		bad["lightRuleIndex"] = MAX_LIGHT_RULES;
	}
	json j;
	j["ledMappings"] = bad.is_null() ? json(std::vector<json>(MAX_LED_MAPPINGS + 1, good)) : json::array({good, bad});
	CHECK_FALSE(pad.LoadProfile(j, DPG_LIGHTS));
	CHECK(raw->sent.empty());
	CHECK(pad.Lights().ledMappings.empty());
}

TEST_CASE("A pad can't be renamed to an empty name", "[profile]")
{
	// Profiles and pad state live under the pad name: an empty one would put
	// them in players/<player>/ and pads/ themselves.
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("Left"), makeIdent(1, 3, 8, 1), {}, {}, {makeSensor(0, 425, 0)});
	raw->sent.clear();

	CHECK_FALSE(pad.SendName(""));
	CHECK_FALSE(pad.SendName("  "));
	json j = {{"name", ""}};
	pad.LoadProfile(j, DPG_ALL);

	CHECK(pad.State().name == "Left");
	CHECK(raw->sent.empty());
}

TEST_CASE("LoadProfile sends one report per changed sensor", "[profile]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 2, IdentificationV2Report::FEATURE_DIGIPOT), {}, {},
	              {makeSensor(0, 425, 0), makeSensor(1, 425, 1)});
	raw->sent.clear();

	// Sensor 0 gets a new threshold and gain (one report, not two); sensor 1
	// gets the values it already has (no report).
	json j;
	j["sensors"] = json::array({{{"threshold", 0.4}, {"resistorValue", 42}},
	                            {{"threshold", pad.Sensor(1)->threshold},
	                             {"releaseThreshold", pad.Sensor(1)->releaseThreshold},
	                             {"resistorValue", 0}}});
	pad.LoadProfile(j, PROFILE_TEST_GROUPS);

	CHECK(raw->sent.size() == 1);
	CHECK(pad.Sensor(0)->threshold == Approx(0.4));
	CHECK(pad.Sensor(0)->resistorValue == 42);
}

TEST_CASE("SetSensor rejects buttons the pad doesn't have", "[profile]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 0, 8, 1), {}, {}, {makeSensor(0, 425, 0)});

	CHECK_FALSE(SetButton(pad, 0, 9));
	CHECK_FALSE(SetButton(pad, 0, -1));
	CHECK(pad.Sensor(0)->button == 1);
}
