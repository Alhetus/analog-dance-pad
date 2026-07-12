#pragma once

// Builders for standing up a PadDevice on a fake backend in unit tests.

#include <cstdint>
#include <cstring>
#include <memory>

#include "support/RecordingBackend.h"

#include <Model/PadDevice.h>
#include <Model/Reporter.h>
#include <Model/Wire.h>

namespace adp
{

// Wraps a fresh RecordingBackend in a real Reporter; `raw` stays valid for as
// long as the returned Reporter lives.
inline std::shared_ptr<Reporter> makeReporter(RecordingBackend*& raw)
{
	auto be = std::make_unique<RecordingBackend>();
	raw = be.get();
	return std::make_shared<Reporter>(std::move(be));
}

inline NameReport makeName(const char* s)
{
	NameReport n;
	size_t len = std::strlen(s);
	n.size = (uint8_t)len;
	std::memcpy(n.name, s, len);
	return n;
}

inline IdentificationV2Report makeIdent(uint16_t major, uint16_t minor, int buttons, int sensors, uint16_t features = 0)
{
	IdentificationV2Report id;
	id.firmwareMajor = WriteU16LE(major);
	id.firmwareMinor = WriteU16LE(minor);
	id.buttonCount = (uint8_t)buttons;
	id.sensorCount = (uint8_t)sensors;
	id.ledCount = 0;
	id.maxSensorValue = WriteU16LE(MAX_SENSOR_VALUE);
	std::memset(id.boardType, 0, BOARD_TYPE_LENGTH);
	std::memcpy(id.boardType, "avr_fsrio1", 10);
	id.features = WriteU16LE(features);
	return id;
}

inline SensorReport makeSensor(int index, int thresholdRaw, int buttonMapping)
{
	SensorReport s;
	s.index = (uint8_t)index;
	s.threshold = WriteU16LE(thresholdRaw);
	s.releaseThreshold = WriteU16LE(thresholdRaw / 2);
	s.buttonMapping = (int8_t)buttonMapping;
	s.resistorValue = 0;
	s.flags = WriteU16LE(0);
	return s;
}

} // namespace adp
