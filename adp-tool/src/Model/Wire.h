#pragma once

// Wire-format conversion helpers shared by the device model and its unit tests.
// Little-endian (de)serialization for the packed HID report structs, plus the
// normalized <-> device sensor-value mapping.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <Model/Reporter.h>

namespace adp
{

inline int ReadU16LE(uint16_le u16)
{
	return u16.bytes[0] | (u16.bytes[1] << 8);
}

inline uint32_t ReadU32LE(uint32_le u32)
{
	return u32.bytes[0] | (u32.bytes[1] << 8) | (u32.bytes[2] << 16) | (uint32_t(u32.bytes[3]) << 24);
}

inline uint16_le WriteU16LE(int value)
{
	uint16_le u16{};
	u16.bytes[0] = value & 0xFF;
	u16.bytes[1] = (value >> 8) & 0xFF;
	return u16;
}

inline uint32_le WriteU32LE(uint32_t value)
{
	uint32_le u32{};
	u32.bytes[0] = value & 0xFF;
	u32.bytes[1] = (value >> 8) & 0xFF;
	u32.bytes[2] = (value >> 16) & 0xFF;
	u32.bytes[3] = (value >> 24) & 0xFF;
	return u32;
}

template <typename T> inline double ToNormalizedSensorValue(T deviceValue)
{
	constexpr double scalar = 1.0 / static_cast<double>(MAX_SENSOR_VALUE);
	return std::clamp(deviceValue * scalar, 0.0, 1.0);
}

inline int ToDeviceSensorValue(double normalizedValue)
{
	// Clamp before rounding: lround of a huge client value (or NaN) is
	// unspecified. !(v > 0) also catches NaN.
	if (!(normalizedValue > 0.0))
		return 0;
	if (normalizedValue >= 1.0)
		return MAX_SENSOR_VALUE;
	return static_cast<int>(std::lround(normalizedValue * MAX_SENSOR_VALUE));
}

} // namespace adp
