#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Model/Wire.h>

using namespace adp;

TEST_CASE("little-endian 16-bit round-trips", "[wire]")
{
	CHECK(ReadU16LE(WriteU16LE(0)) == 0);
	CHECK(ReadU16LE(WriteU16LE(1)) == 1);
	CHECK(ReadU16LE(WriteU16LE(0x1234)) == 0x1234);
	CHECK(ReadU16LE(WriteU16LE(0xFFFF)) == 0xFFFF);

	// Byte order is explicitly little-endian.
	auto v = WriteU16LE(0xABCD);
	CHECK(v.bytes[0] == 0xCD);
	CHECK(v.bytes[1] == 0xAB);
}

TEST_CASE("little-endian 32-bit round-trips", "[wire]")
{
	CHECK(ReadU32LE(WriteU32LE(0u)) == 0u);
	CHECK(ReadU32LE(WriteU32LE(0x89ABCDEFu)) == 0x89ABCDEFu);
	CHECK(ReadU32LE(WriteU32LE(0xFFFFFFFFu)) == 0xFFFFFFFFu);

	// High bit must not sign-extend (regression: uint32_t cast on byte 3).
	auto v = WriteU32LE(0x80000000u);
	CHECK(ReadU32LE(v) == 0x80000000u);
	CHECK(v.bytes[3] == 0x80);
}

TEST_CASE("float32 round-trips via bit_cast", "[wire]")
{
	CHECK(ReadF32LE(WriteF32LE(0.0f)) == 0.0f);
	CHECK(ReadF32LE(WriteF32LE(1.0f)) == 1.0f);
	CHECK(ReadF32LE(WriteF32LE(-3.5f)) == -3.5f);
	CHECK(ReadF32LE(WriteF32LE(1234.5f)) == 1234.5f);
}

TEST_CASE("sensor value normalization clamps to [0,1]", "[wire]")
{
	CHECK(ToNormalizedSensorValue(0) == 0.0);
	CHECK(ToNormalizedSensorValue(MAX_SENSOR_VALUE) == Catch::Approx(1.0));
	// Out-of-range inputs are clamped, never negative or > 1.
	CHECK(ToNormalizedSensorValue(-100) == 0.0);
	CHECK(ToNormalizedSensorValue(MAX_SENSOR_VALUE * 2) == 1.0);
	// Never exceeds the [0,1] bounds regardless of input.
	CHECK(ToNormalizedSensorValue(MAX_SENSOR_VALUE) <= 1.0);
}

TEST_CASE("device sensor value mapping clamps to [0,MAX]", "[wire]")
{
	CHECK(ToDeviceSensorValue(0.0) == 0);
	CHECK(ToDeviceSensorValue(1.0) == MAX_SENSOR_VALUE);
	CHECK(ToDeviceSensorValue(-1.0) == 0);
	CHECK(ToDeviceSensorValue(2.0) == MAX_SENSOR_VALUE);
}

TEST_CASE("raw little-endian bytes decode with the right weights", "[wire]")
{
	// Not a round-trip: pin that byte[0] is the low byte, byte[1] the high byte.
	CHECK(ReadU16LE(uint16_le{{0xCD, 0xAB}}) == 0xABCD);
	CHECK(ReadU32LE(uint32_le{{0x01, 0x02, 0x03, 0x04}}) == 0x04030201u);
}

TEST_CASE("WriteU16LE truncates to the low 16 bits", "[wire]")
{
	// Values above 0xFFFF drop their high bits (only two bytes are written).
	auto v = WriteU16LE(0x12345);
	CHECK(v.bytes[0] == 0x45);
	CHECK(v.bytes[1] == 0x23);
	CHECK(ReadU16LE(v) == 0x2345);
}

TEST_CASE("ToDeviceSensorValue rounds half away from zero", "[wire]")
{
	// 0.25 * 850 == 212.5, which lround pushes up to 213.
	CHECK(ToDeviceSensorValue(0.25) == 213);
}
