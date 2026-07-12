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
}

TEST_CASE("PadDevice normalizes sensor thresholds and maps buttons", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	std::vector<SensorReport> sensors = {
		makeSensor(0, 425, 2),   // button 2 < 8 buttons -> 1-based button 3
		makeSensor(1, 850, 10),  // button 10 >= 8 buttons -> unmapped (0)
		makeSensor(2, 0, 0),     // button 0 < 8 buttons -> 1-based button 1
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

	CHECK_FALSE(pad.SetThreshold(99, 0.5, 0.4));
	CHECK_FALSE(pad.SetThreshold(-1, 0.5, 0.4));
	CHECK_FALSE(pad.SetAdcConfig(99, 5));
	CHECK(raw->sent.empty());
}

TEST_CASE("PadDevice picks the report by firmware version", "[paddevice]")
{
	std::vector<SensorReport> sensors = { makeSensor(0, 400, 0), makeSensor(1, 400, 1) };

	SECTION("v1.3+ sends a SensorReport")
	{
		RecordingBackend* raw = nullptr;
		auto rep = makeReporter(raw);
		PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 3, 8, 2), {}, {}, sensors);
		raw->sent.clear();

		CHECK(pad.SetThreshold(0, 0.5, 0.4)); // Send() alone, no device echo needed
		REQUIRE(raw->sent.size() == 1);
		CHECK(raw->sent[0][0] == REPORT_SENSOR);
	}

	SECTION("pre-v1.3 sends a PadConfigurationReport")
	{
		RecordingBackend* raw = nullptr;
		auto rep = makeReporter(raw);
		PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 1, 8, 2), {}, {}, sensors);
		raw->sent.clear();

		PadConfigurationReport echo;
		raw->QueueGet(echo); // SendAndGet needs a device echo to succeed
		CHECK(pad.SetThreshold(0, 0.5, 0.4));
		REQUIRE(raw->sent.size() == 1);
		CHECK(raw->sent[0][0] == REPORT_PAD_CONFIGURATION);
	}
}

TEST_CASE("PadDevice clamps the release threshold to [0.01, 1.0]", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 1, 8, 2),
		{}, {}, { makeSensor(0, 400, 0), makeSensor(1, 400, 1) });

	PadConfigurationReport echo;
	raw->QueueGet(echo);
	pad.SetReleaseThreshold(5.0);
	CHECK(pad.State().releaseThreshold == Approx(1.0));

	raw->QueueGet(echo);
	pad.SetReleaseThreshold(-1.0);
	CHECK(pad.State().releaseThreshold == Approx(0.01));
}

TEST_CASE("PadDevice averages polled samples and derives pressed state", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);
	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 0, 4, 2),
		{}, {}, { makeSensor(0, 400, 0), makeSensor(1, 400, 1) });
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

TEST_CASE("SendPadConfiguration never writes past the legacy sensor count", "[paddevice]")
{
	RecordingBackend* raw = nullptr;
	auto rep = makeReporter(raw);

	std::vector<SensorReport> sensors;
	for (int i = 0; i < 20; ++i) // more than SENSOR_COUNT_V1 (12)
		sensors.push_back(makeSensor(i, 400, 0));

	PadDevice pad(rep, "t", makeName("x"), makeIdent(1, 1, 8, 20), {}, {}, sensors);
	raw->sent.clear();

	PadConfigurationReport echo;
	raw->QueueGet(echo);
	CHECK(pad.SendPadConfiguration());
	REQUIRE(raw->sent.size() == 1);
	CHECK(raw->sent[0].size() == sizeof(PadConfigurationReport));
	CHECK(raw->sent[0][0] == REPORT_PAD_CONFIGURATION);
}
