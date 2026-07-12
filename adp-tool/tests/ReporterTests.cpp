#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "support/RecordingBackend.h"

#include <Model/Reporter.h>
#include <Model/Wire.h>

using namespace adp;

static std::unique_ptr<RecordingBackend> makeBackend(RecordingBackend*& raw)
{
	auto be = std::make_unique<RecordingBackend>();
	raw = be.get();
	return be;
}

TEST_CASE("Get(SensorValuesReport) returns SUCCESS on exact size and parses values", "[reporter]")
{
	const int numSensors = 4;
	const int expected = (int)sizeof(uint8_t) + (int)sizeof(uint16_le) + (int)sizeof(uint16_le) * numSensors;

	SensorValuesReport in;
	in.buttonBits = WriteU16LE(0b101);
	for (int i = 0; i < numSensors; ++i)
		in.sensorValues[i] = WriteU16LE(100 + i);

	RecordingBackend* raw = nullptr;
	auto be = makeBackend(raw);
	raw->QueueRead(&in, sizeof(in), expected);
	Reporter rep(std::move(be));

	SensorValuesReport out;
	CHECK(rep.Get(out, numSensors) == ReadDataResult::SUCCESS);
	CHECK(ReadU16LE(out.buttonBits) == 0b101);
	CHECK(ReadU16LE(out.sensorValues[0]) == 100);
	CHECK(ReadU16LE(out.sensorValues[3]) == 103);
}

TEST_CASE("Get(SensorValuesReport) maps zero bytes to NO_DATA", "[reporter]")
{
	RecordingBackend* raw = nullptr;
	auto be = makeBackend(raw);
	raw->QueueReadCount(0);
	Reporter rep(std::move(be));

	SensorValuesReport out;
	CHECK(rep.Get(out, 4) == ReadDataResult::NO_DATA);
}

TEST_CASE("Get(SensorValuesReport) maps wrong/negative size to FAILURE", "[reporter]")
{
	const int numSensors = 4;
	const int expected = (int)sizeof(uint8_t) + (int)sizeof(uint16_le) + (int)sizeof(uint16_le) * numSensors;

	SECTION("one byte too many")
	{
		RecordingBackend* raw = nullptr;
		auto be = makeBackend(raw);
		SensorValuesReport in;
		raw->QueueRead(&in, sizeof(in), expected + 1);
		Reporter rep(std::move(be));

		SensorValuesReport out;
		CHECK(rep.Get(out, numSensors) == ReadDataResult::FAILURE);
	}

	SECTION("negative (HID error)")
	{
		RecordingBackend* raw = nullptr;
		auto be = makeBackend(raw);
		raw->QueueReadCount(-1);
		Reporter rep(std::move(be));

		SensorValuesReport out;
		CHECK(rep.Get(out, numSensors) == ReadDataResult::FAILURE);
	}
}

TEST_CASE("GetFeatureReport accepts an exact-size read", "[reporter]")
{
	NameReport in;
	in.size = 3;
	std::memcpy(in.name, "abc", 3);

	RecordingBackend* raw = nullptr;
	auto be = makeBackend(raw);
	raw->QueueGet(in);
	Reporter rep(std::move(be));

	NameReport out;
	CHECK(rep.Get(out));
	CHECK(out.size == 3);
	CHECK(std::string((const char*)out.name, 3) == "abc");
}

TEST_CASE("GetFeatureReport rejects a wrong-size read", "[reporter]")
{
	NameReport in;
	RecordingBackend* raw = nullptr;
	auto be = makeBackend(raw);
	raw->QueueRead(&in, sizeof(in), (int)sizeof(in) - 1); // one byte short
	Reporter rep(std::move(be));

	NameReport out;
	CHECK_FALSE(rep.Get(out));
}

TEST_CASE("Send serializes the report to the wire and captures the bytes", "[reporter]")
{
	RecordingBackend* raw = nullptr;
	auto be = makeBackend(raw);
	Reporter rep(std::move(be));

	SensorReport report;
	report.index = 5;
	report.threshold = WriteU16LE(300);
	report.buttonMapping = 2;
	report.resistorValue = 7;

	CHECK(rep.Send(report));
	REQUIRE(raw->sent.size() == 1);

	const auto& bytes = raw->sent[0];
	REQUIRE(bytes.size() == sizeof(SensorReport));
	CHECK(bytes[0] == REPORT_SENSOR);           // reportId
	CHECK(bytes[1] == 5);                       // index
	CHECK((bytes[2] | (bytes[3] << 8)) == 300); // threshold, little-endian
	CHECK((int8_t)bytes[6] == 2);               // buttonMapping
	CHECK(bytes[7] == 7);                       // resistorValue
}

TEST_CASE("Send returns false on a short feature-report write", "[reporter]")
{
	RecordingBackend* raw = nullptr;
	auto be = makeBackend(raw);
	raw->sendFeatureReturn = 1; // fewer than sizeof(SensorReport)
	Reporter rep(std::move(be));

	SensorReport report;
	CHECK_FALSE(rep.Send(report));
}

TEST_CASE("WriteData honors performErrorCheck", "[reporter]")
{
	SECTION("SendSaveConfiguration reports a failed write")
	{
		RecordingBackend* raw = nullptr;
		auto be = makeBackend(raw);
		raw->writeReturn = -1;
		Reporter rep(std::move(be));
		CHECK_FALSE(rep.SendSaveConfiguration());
	}

	SECTION("SendSaveConfiguration succeeds on a good write")
	{
		RecordingBackend* raw = nullptr;
		auto be = makeBackend(raw);
		Reporter rep(std::move(be));
		CHECK(rep.SendSaveConfiguration());
	}
}
