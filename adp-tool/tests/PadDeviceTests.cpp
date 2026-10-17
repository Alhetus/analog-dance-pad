#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstring>
#include <memory>
#include <vector>

#include "support/PadFixture.h"
#include "support/RecordingBackend.h"

#include <Model/PadDevice.h>
#include <Model/Reporter.h>
#include <Model/Wire.h>

using namespace adp;
using Catch::Approx;

// --- tests ----------------------------------------------------------------

TEST_CASE("PadDevice clamps a hostile sensor count to SENSOR_COUNT_MAX", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 0, 8, 200), {}, {}, {});

	CHECK(pad.State().numSensors == SENSOR_COUNT_MAX);
	CHECK(pad.Sensor(SENSOR_COUNT_MAX - 1) != nullptr);
	CHECK(pad.Sensor(SENSOR_COUNT_MAX) == nullptr);
}

TEST_CASE("PadDevice keeps a valid name and rejects an over-long one", "[paddevice]")
{
	SECTION("valid name is kept")
	{
		RecordingBackend* raw = nullptr;
		auto rep = makeReporter(raw);
		PadDevice pad(rep, "t", makeName("Hello"), makeIdent(1, 0, 8, 2), {}, {}, {});
		CHECK(pad.State().name == "Hello");
	}

	SECTION("over-long name falls back to Unknown")
	{
		RecordingBackend* raw = nullptr;
		auto rep = makeReporter(raw);
		NameReport longName;
		longName.size = 200; // > MAX_NAME_LENGTH
		PadDevice pad(rep, "t", longName, makeIdent(1, 0, 8, 2), {}, {}, {});
		CHECK(pad.State().name == "Unknown");
	}

	SECTION("empty name falls back to Unknown (profiles are stored under it)")
	{
		RecordingBackend* raw = nullptr;
		auto rep = makeReporter(raw);
		PadDevice pad(rep, "t", makeName(""), makeIdent(1, 0, 8, 2), {}, {}, {});
		CHECK(pad.State().name == "Unknown");
	}
}

TEST_CASE("PadDevice writes the device's sensor flags back unchanged", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	SensorReport sensor = makeSensor(0, 400, 0);
	sensor.flags = WriteU16LE(SensorReport::ADC_DISABLED);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1), {}, {}, {sensor});
	raw->sent.clear();

	REQUIRE(SetThreshold(pad, 0, 0.5, 0.4));
	REQUIRE(raw->sent.size() == 1);
	REQUIRE(raw->sent[0].size() == sizeof(SensorReport));
	SensorReport out;
	std::memcpy(&out, raw->sent[0].data(), sizeof(out));
	CHECK(ReadU16LE(out.flags) == SensorReport::ADC_DISABLED);
}

TEST_CASE("PadDevice normalizes sensor thresholds and maps buttons", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	std::vector<SensorReport> sensors = {
	    makeSensor(0, 425, 2),  // button 2 < 8 buttons -> 1-based button 3
	    makeSensor(1, 850, 10), // button 10 >= 8 buttons -> unmapped (0)
	    makeSensor(2, 0, 0),    // button 0 < 8 buttons -> 1-based button 1
	};
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 0, 8, 3), {}, {}, sensors);

	CHECK(pad.Sensor(0)->threshold == Approx(0.5));
	CHECK(pad.Sensor(0)->button == 3);
	CHECK(pad.Sensor(1)->button == 0);
	CHECK(pad.Sensor(2)->button == 1);
}

TEST_CASE("PadDevice rejects out-of-range sensor indices without touching the wire", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 0, 8, 4), {}, {}, {});
	raw->sent.clear(); // drop the ctor's release-mode probe

	CHECK_FALSE(SetThreshold(pad, 99, 0.5, 0.4));
	CHECK_FALSE(SetThreshold(pad, -1, 0.5, 0.4));
	CHECK_FALSE(SetGain(pad, 99, 5));
	CHECK(raw->sent.empty());
}

TEST_CASE("PadDevice sends a sensor edit as one SensorReport", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 2), {}, {},
	              {makeSensor(0, 400, 0), makeSensor(1, 400, 1)});
	raw->sent.clear();

	CHECK(SetThreshold(pad, 0, 0.5, 0.4)); // Send() alone, no device echo needed
	REQUIRE(raw->sent.size() == 1);
	CHECK(raw->sent[0][0] == REPORT_SENSOR);
}

TEST_CASE("A release mode change is saved to the pad", "[paddevice]")
{
	// The firmware keeps the mode in RAM until the next save-configuration.
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1), {}, {}, {makeSensor(0, 425, 0)});
	REQUIRE_FALSE(pad.HasUnsavedChanges());

	CHECK(pad.SetReleaseMode(RELEASE_INDIVIDUAL));
	CHECK(pad.HasUnsavedChanges());
}

TEST_CASE("Release mode governs the stored release threshold", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	// v1.3+ so per-sensor SensorReports are used (Send() alone, no echo needed).
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1), {}, {}, {makeSensor(0, 425, 0)});
	raw->sent.clear();

	SECTION("individual mode honors the requested per-sensor release")
	{
		CHECK(pad.SetReleaseMode(RELEASE_INDIVIDUAL));
		// 0.2*850=170 survives quantization exactly and differs from the ctor ratio
		// (0.5*~0.499=~0.25), proving the requested value is kept verbatim.
		CHECK(SetThreshold(pad, 0, 0.5, 0.2));
		CHECK(pad.Sensor(0)->threshold == Approx(0.5));
		CHECK(pad.Sensor(0)->releaseThreshold == Approx(0.2));
	}

	SECTION("none mode forces release to equal the threshold")
	{
		CHECK(pad.SetReleaseMode(RELEASE_NONE));
		// Switching to None immediately re-derives release == threshold.
		CHECK(pad.Sensor(0)->releaseThreshold == Approx(pad.Sensor(0)->threshold));
		// A later threshold edit keeps release pinned to the threshold, ignoring
		// whatever release the caller passes.
		CHECK(SetThreshold(pad, 0, 0.7, 0.2));
		CHECK(pad.Sensor(0)->threshold == Approx(0.7));
		CHECK(pad.Sensor(0)->releaseThreshold == Approx(0.7));
	}

	SECTION("legacy global mode is treated as per-sensor")
	{
		// Global mode was removed; a device still reporting it must honor per-sensor
		// release edits rather than deriving from a shared ratio.
		CHECK(pad.SetReleaseMode(RELEASE_GLOBAL));
		CHECK(SetThreshold(pad, 0, 0.5, 0.2));
		CHECK(pad.Sensor(0)->releaseThreshold == Approx(0.2));
	}

	SECTION("release threshold is clamped to never exceed the press threshold")
	{
		CHECK(pad.SetReleaseMode(RELEASE_INDIVIDUAL));
		// Request a release above the threshold; it must be pinned to the threshold.
		CHECK(SetThreshold(pad, 0, 0.5, 0.9));
		CHECK(pad.Sensor(0)->releaseThreshold == Approx(0.5));
	}
}

TEST_CASE("PadDevice clamps a hostile button count to the 16-bit button mask", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 200, 1), {}, {}, {makeSensor(0, 425, 0)});

	CHECK(pad.State().numButtons == MAX_BUTTON_COUNT);
	CHECK(SetButton(pad, 0, MAX_BUTTON_COUNT));
	CHECK_FALSE(SetButton(pad, 0, MAX_BUTTON_COUNT + 1));
}

TEST_CASE("PadDevice clamps gain to one digipot byte", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1), {}, {}, {makeSensor(0, 425, 0)});

	CHECK(SetGain(pad, 0, 300));
	CHECK(pad.Sensor(0)->resistorValue == 255);
	CHECK(SetGain(pad, 0, -5));
	CHECK(pad.Sensor(0)->resistorValue == 0);
}

TEST_CASE("PadDevice keeps its state when the device refuses a sensor report", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1), {}, {}, {makeSensor(0, 425, 0)});
	raw->sendFeatureReturn = -1;

	CHECK_FALSE(SetThreshold(pad, 0, 0.2, 0.1));
	CHECK(pad.Sensor(0)->threshold == Approx(0.5));
	CHECK_FALSE(SetButton(pad, 0, 4));
	CHECK(pad.Sensor(0)->button == 1);
	CHECK_FALSE(pad.HasUnsavedChanges());
}

TEST_CASE("PadDevice sends nothing for a sensor edit the device already holds", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1), {}, {}, {makeSensor(0, 425, 0)});
	raw->sent.clear();

	const SensorState s = *pad.Sensor(0);
	CHECK(SetThreshold(pad, 0, s.threshold, s.releaseThreshold));
	CHECK(raw->sent.empty());
}

TEST_CASE("PadDevice averages polled samples and derives pressed state", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 0, 4, 2), {}, {},
	              {makeSensor(0, 400, 0), makeSensor(1, 400, 1)});
	raw->sent.clear();

	const int expected = 1 + 2 + 2 * 2; // reportId + buttonBits + 2 sensors

	SensorValuesReport a;
	a.buttonBits = WriteU16LE(0b01); // button 1 pressed (bit 0)
	a.sensorValues[0] = WriteU16LE(100);
	a.sensorValues[1] = WriteU16LE(200);
	raw->QueueRead(&a, sizeof(a), expected);

	SensorValuesReport b;
	b.buttonBits = WriteU16LE(0b01);
	b.sensorValues[0] = WriteU16LE(300);
	b.sensorValues[1] = WriteU16LE(400);
	raw->QueueRead(&b, sizeof(b), expected);

	raw->QueueReadCount(0); // NO_DATA ends the read loop

	CHECK(pad.PollSensors());

	// Averaged: sensor0 = (100+300)/2 = 200, sensor1 = (200+400)/2 = 300.
	CHECK(pad.Sensor(0)->value == Approx(200.0 / MAX_SENSOR_VALUE));
	CHECK(pad.Sensor(1)->value == Approx(300.0 / MAX_SENSOR_VALUE));
	// button 1 (bit 0) is pressed; button 2 (bit 1) is not.
	CHECK(pad.Sensor(0)->pressed);
	CHECK_FALSE(pad.Sensor(1)->pressed);
}

TEST_CASE("PollSensors skips a foreign report instead of disconnecting", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 0, 4, 2), {}, {}, {makeSensor(0, 400, 0)});

	SensorValuesReport foreign;
	foreign.reportId = REPORT_UNUSED_JOYSTICK;
	raw->QueueRead(&foreign, sizeof(foreign), 3);

	SensorValuesReport ours;
	ours.sensorValues[0] = WriteU16LE(425);
	raw->QueueRead(&ours, sizeof(ours), 1 + 2 + 2 * 2);
	raw->QueueReadCount(0);

	CHECK(pad.PollSensors());
	CHECK(pad.Sensor(0)->value == Approx(0.5));

	raw->QueueReadCount(-1);
	CHECK_FALSE(pad.PollSensors());
}

TEST_CASE("SaveChanges keeps the change unsaved when the save fails", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1), {}, {}, {makeSensor(0, 425, 0)});
	CHECK(SetThreshold(pad, 0, 0.7, 0.2));
	REQUIRE(pad.HasUnsavedChanges());

	raw->writeReturn = -1;
	pad.SaveChanges();
	CHECK(pad.HasUnsavedChanges());
	CHECK(pad.SaveFailed());

	raw->writeReturn.reset();
	pad.SaveChanges();
	CHECK_FALSE(pad.HasUnsavedChanges());
	CHECK_FALSE(pad.SaveFailed());
}

TEST_CASE("PadDevice treats any negative button mapping as unmapped", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 2), {}, {},
	              {makeSensor(0, 425, -5), makeSensor(1, 425, -1)});
	CHECK(pad.Sensor(0)->button == 0);
	CHECK(pad.Sensor(1)->button == 0);
	// Still editable (a negative button used to make every edit fail).
	CHECK(SetThreshold(pad, 0, 0.6, 0.3));
}

TEST_CASE("PadDevice treats an unknown release mode as per-sensor", "[paddevice]")
{
	for (const uint32_t reported : {0u, 1u, 2u, 7u})
	{
		RecordingBackend* raw = nullptr;
		auto rep = makeReporter(raw);
		SetPropertyReport mode;
		mode.propertyId = WriteU32LE(SetPropertyReport::SPID_RELEASE_MODE);
		mode.propertyValue = WriteU32LE(reported);
		raw->QueueGet(mode);
		PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1), {}, {}, {makeSensor(0, 425, 0)});
		CHECK(pad.State().releaseMode == (reported == 0 ? RELEASE_NONE : RELEASE_INDIVIDUAL));
	}
}

TEST_CASE("The release threshold never goes below the noise floor", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 1), {}, {}, {makeSensor(0, 425, 0)});
	REQUIRE(pad.SetReleaseMode(RELEASE_INDIVIDUAL));

	CHECK(SetThreshold(pad, 0, 0.5, 0.0));
	CHECK(ToDeviceSensorValue(pad.Sensor(0)->releaseThreshold) == ToDeviceSensorValue(MIN_RELEASE_THRESHOLD));
	// A threshold below the floor takes the release down with it, never above.
	CHECK(SetThreshold(pad, 0, 0.005, 0.0));
	CHECK(pad.Sensor(0)->releaseThreshold <= pad.Sensor(0)->threshold);
}

TEST_CASE("A pad name that isn't UTF-8 still serializes", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("\xC5sa\xFF"), makeIdent(1, 3, 8, 1), {}, {}, {makeSensor(0, 425, 0)});
	CHECK(pad.State().name == "\xEF\xBF\xBDsa\xEF\xBF\xBD");
	CHECK_NOTHROW(json(pad.State().name).dump());
}
