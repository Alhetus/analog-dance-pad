#include <catch2/catch_test_macros.hpp>

#include <Model/Device.h>

using namespace adp;

// HandleClientMessage runs on the device-I/O thread and must be fully
// defensive: no device is connected here (Device::Init is never called in
// tests), so every message should be parsed/validated and then ignored without
// throwing. The config-apply path itself is covered by ProfileTests against
// PadDevice directly.
TEST_CASE("HandleClientMessage ignores bad input without throwing", "[handlemessage]")
{
	CHECK_NOTHROW(Device::HandleClientMessage("not json at all"));
	CHECK_NOTHROW(Device::HandleClientMessage(""));
	CHECK_NOTHROW(Device::HandleClientMessage("[1,2,3]"));          // valid JSON, not an object
	CHECK_NOTHROW(Device::HandleClientMessage("42"));               // valid JSON, not an object
	CHECK_NOTHROW(Device::HandleClientMessage("{\"sensors\":[]}")); // object, but no device
	CHECK_NOTHROW(Device::HandleClientMessage("{\"releaseMode\":1}"));
	CHECK_NOTHROW(Device::HandleClientMessage("{\"releaseMode\":99}"));    // out of range
	CHECK_NOTHROW(Device::HandleClientMessage("{\"calibrateSensor\":0}"));
	CHECK_NOTHROW(Device::HandleClientMessage("{\"unhandled\":true}"));
}
