#include <catch2/catch_test_macros.hpp>

#include <Model/Device.h>
#include <Model/Wire.h>

using namespace adp;

TEST_CASE("SensorState::ToReport carries index, thresholds and resistor", "[sensor]")
{
	SensorState s;
	s.threshold = 0.5;
	s.releaseThreshold = 0.25;
	s.resistorValue = 42;
	s.button = 3;

	SensorReport r = s.ToReport(7);

	CHECK(r.index == 7);
	CHECK(ReadU16LE(r.threshold) == ToDeviceSensorValue(0.5));
	CHECK(ReadU16LE(r.releaseThreshold) == ToDeviceSensorValue(0.25));
	CHECK(r.resistorValue == 42);
	// button is 1-based; the wire mapping is button-1.
	CHECK(r.buttonMapping == 2);
}

TEST_CASE("SensorState::ToReport maps unmapped button (0) to 0xFF", "[sensor]")
{
	SensorState s;
	s.button = 0; // zero means unmapped.

	SensorReport r = s.ToReport(0);

	// buttonMapping is int8_t; the code writes 0xFF, which stores as -1.
	CHECK(r.buttonMapping == -1);
}
