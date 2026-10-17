#pragma once

// The connected pad's in-memory state machine, extracted from Device.cpp so it
// can be unit-tested with a fake Reporter backend (no real HID device). All
// hidapi access still goes through the injected Reporter; PadDevice itself only
// owns state and wire-format translation.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <Model/Device.h>
#include <Model/Firmware.h>
#include <Model/Reporter.h>
#include <Model/Wire.h>

namespace adp
{

enum LedMappingFlags
{
	LMF_ENABLED = 1 << 0,
};

enum LightRuleFlags
{
	LRF_ENABLED = 1 << 0,
	LRF_FADE_ON = 1 << 1,
	LRF_FADE_OFF = 1 << 2,
};

using DevicePath = std::string;

inline bool IsBitSet(int bits, int index)
{
	return (bits & (1 << index)) != 0;
}

inline RgbColor ToRgbColor(color24 color)
{
	return {color.red, color.green, color.blue};
}

inline color24 ToColor24(RgbColor color)
{
	return {color.red, color.green, color.blue};
}

// The device is untrusted input: clamp every count it reports to what the
// report structs and the 16-bit button mask can hold. Applied once, before
// anything is indexed by these counts (see ConnectToDeviceStage2 / PadDevice).
inline void ClampIdentification(IdentificationReport& id)
{
	id.sensorCount = (uint8_t)std::min<int>(id.sensorCount, SENSOR_COUNT_MAX);
	id.buttonCount = (uint8_t)std::min<int>(id.buttonCount, MAX_BUTTON_COUNT);
}

struct PollingData
{
	int readsSinceLastUpdate = 0;
	int pollingRate = 0;
	std::chrono::time_point<std::chrono::steady_clock> lastUpdate;
};

class PadDevice
{
  public:
	PadDevice(std::shared_ptr<Reporter> reporter, const char* path, const NameReport& name,
	          IdentificationV2Report identification, const std::vector<LightRuleReport>& lightRules,
	          const std::vector<LedMappingReport>& ledMappings, const std::vector<SensorReport>& sensors)
	    : myReporter(std::move(reporter)), myPath(path)
	{
		ClampIdentification(identification);
		const int sensorCount = identification.sensorCount;

		mySensors = std::vector<SensorState>(sensorCount);

		UpdateName(name);
		myPad.maxNameLength = MAX_NAME_LENGTH;

		myPad.numButtons = identification.buttonCount;
		myPad.numSensors = sensorCount;

		// Board type is a fixed-width, not-necessarily-NUL-terminated field.
		{
			char boardType[BOARD_TYPE_LENGTH + 1] = {};
			std::memcpy(boardType, identification.boardType, BOARD_TYPE_LENGTH);
			myPad.boardType = BoardTypeStruct(boardType);
		}

		myPad.firmwareVersion.major = ReadU16LE(identification.firmwareMajor);
		myPad.firmwareVersion.minor = ReadU16LE(identification.firmwareMinor);

		uint16_t features = ReadU16LE(identification.features);
		myPad.featureDebug = (features & IdentificationV2Report::FEATURE_DEBUG) != 0;
		myPad.featureDigipot = (features & IdentificationV2Report::FEATURE_DIGIPOT) != 0;
		myPad.featureLights = (features & IdentificationV2Report::FEATURE_LIGHTS) != 0;

		for (auto sensor : sensors)
		{
			UpdateSensor(sensor);
		}

		// Pre-1.3 firmware keeps one release ratio for the whole pad (its sensors
		// were built as threshold*ratio); recover it so SendPadConfiguration
		// writes it back unchanged instead of resetting it.
		if (sensorCount > 0 && mySensors[0].threshold > 0)
			myPad.releaseThreshold = mySensors[0].releaseThreshold / mySensors[0].threshold;
		if (!myPad.firmwareVersion.IsNewer({1, 2}) && myPad.firmwareVersion.IsNewer({1, 1}))
			myPad.featureLights = (bool)(identification.ledCount > 0);

		try
		{
			SetPropertyReport report;
			report.propertyId = WriteU32LE(SetPropertyReport::SPID_SELECTED_PROPERTY);
			report.propertyValue = WriteU32LE(SetPropertyReport::SPID_RELEASE_MODE);

			if (myReporter->SendAndGet(report))
			{
				int resultId = ReadU32LE(report.propertyId);
				if (resultId != SetPropertyReport::SPID_RELEASE_MODE)
				{
					std::printf("Fetching release mode bugged (1)\n");
				}
				else
				{
					myPad.releaseMode = (ReleaseMode)ReadU32LE(report.propertyValue);
					// Global release mode has been removed; a device still reporting
					// it is treated as per-sensor (its stored per-sensor release
					// values, previously threshold*ratio, carry over unchanged).
					if (myPad.releaseMode == RELEASE_GLOBAL)
						myPad.releaseMode = RELEASE_INDIVIDUAL;
				}
			}
		}
		catch (...)
		{
			std::printf("Fetching release mode failed\n");
		}

		UpdateLightsConfiguration(lightRules, ledMappings);
		myPollingData.lastUpdate = std::chrono::steady_clock::now();
	}

	~PadDevice() = default;

	// True if the index is a valid position in mySensors. Used to reject
	// out-of-range indices coming from the device or from client profiles.
	bool ValidSensorIndex(int index) const { return index >= 0 && index < (int)mySensors.size(); }

	void UpdateName(const NameReport& report)
	{
		// An empty name would put profiles and pad state in the parent dirs
		// (they are stored under the pad name), so it is replaced too.
		if (report.size > 0 && report.size <= MAX_NAME_LENGTH)
		{
			myPad.name = "";
			myPad.name.append((const char*)report.name, (size_t)report.size);
		}
		else
		{
			myPad.name = "Unknown";
		}

		myChanges |= DCF_NAME;
	}

	void UpdateLightRule(const LightRuleReport& report)
	{
		if (report.flags & LRF_ENABLED)
		{
			LightRule result;
			result.fadeOn = (report.flags & LRF_FADE_ON) != 0;
			result.fadeOff = (report.flags & LRF_FADE_OFF) != 0;
			result.onColor = ToRgbColor(report.onColor);
			result.onFadeColor = ToRgbColor(report.onFadeColor);
			result.offColor = ToRgbColor(report.offColor);
			result.offFadeColor = ToRgbColor(report.offFadeColor);
			myLights.lightRules[report.lightRuleIndex] = result;
		}
		else
		{
			myLights.lightRules.erase(report.lightRuleIndex);
		}
	}

	void UpdateLedMapping(const LedMappingReport& report)
	{
		if (report.flags & LMF_ENABLED)
		{
			LedMapping result;
			result.lightRuleIndex = report.lightRuleIndex;
			result.sensorIndex = report.sensorIndex;
			result.ledIndexBegin = report.ledIndexBegin;
			result.ledIndexEnd = report.ledIndexEnd;
			myLights.ledMappings[report.ledMappingIndex] = result;
		}
		else
		{
			myLights.ledMappings.erase(report.ledMappingIndex);
		}
	}

	void UpdateLightsConfiguration(const std::vector<LightRuleReport>& lightRules,
	                               const std::vector<LedMappingReport>& ledMappings)
	{
		for (auto& report : lightRules)
			UpdateLightRule(report);

		for (auto& report : ledMappings)
			UpdateLedMapping(report);
	}

	// Reads pending sensor samples from the device. Called once per tick on the
	// single device-I/O thread (there is no separate sensor thread anymore, so
	// mySensors/myPollingData have exactly one writer). Returns false on a HID
	// read failure, which the caller treats as a disconnect.
	bool PollSensors()
	{
		SensorValuesReport report;
		std::vector<int> aggregateValues = std::vector<int>(myPad.numSensors, 0);

		int pressedButtons = 0;
		int inputsRead = 0;

		for (int readsLeft = 100; readsLeft > 0; --readsLeft)
		{
			switch (myReporter->Get(report, myPad.numSensors))
			{
			case ReadDataResult::SUCCESS:
				pressedButtons |= ReadU16LE(report.buttonBits);
				for (int i = 0; i < myPad.numSensors; ++i)
					aggregateValues[i] += ReadU16LE(report.sensorValues[i]);
				++inputsRead;
				break;

			case ReadDataResult::NO_DATA:
				readsLeft = 0;
				break;

			case ReadDataResult::SKIPPED:
				break;

			case ReadDataResult::FAILURE:
				return false;
			}
		}

		if (inputsRead > 0)
		{
			for (int i = 0; i < myPad.numSensors; ++i)
			{
				auto button = mySensors[i].button;
				auto value = (double)aggregateValues[i] / (double)inputsRead;
				mySensors[i].pressed = button > 0 && IsBitSet(pressedButtons, button - 1);
				mySensors[i].value = ToNormalizedSensorValue(value);
			}
			myPollingData.readsSinceLastUpdate += inputsRead;
		}

		auto now = std::chrono::steady_clock::now();
		if (now > myPollingData.lastUpdate + std::chrono::seconds(1))
		{
			auto dt = std::chrono::duration<double>(now - myPollingData.lastUpdate).count();
			myPollingData.pollingRate = (int)std::lround(myPollingData.readsSinceLastUpdate / dt);
			myPollingData.readsSinceLastUpdate = 0;
			myPollingData.lastUpdate = now;
		}

		// Use the loop to save changes if needed
		if (myHasUnsavedChanges &&
		    std::chrono::duration_cast<std::chrono::milliseconds>(now - myLastPendingChange).count() > 2000)
		{
			SaveChanges();
		}

		return true;
	}

	// The releaseThreshold to store for a sensor with press threshold
	// `threshold` under the active release mode. None means no hysteresis
	// (release tracks the press threshold); per-sensor honors `requested`.
	double ReleaseForMode(double threshold, double requested) const
	{
		if (myPad.releaseMode == RELEASE_NONE)
			return threshold;
		// The release threshold must never exceed the press threshold, or hysteresis
		// breaks (the button would release the instant it presses). Clamp it down.
		return std::clamp(requested, 0.0, threshold);
	}

	bool ValidButton(int button) const { return button >= 0 && button <= myPad.numButtons; }

	bool SetReleaseMode(ReleaseMode mode)
	{
		SetPropertyReport report;
		report.propertyId = WriteU32LE(SetPropertyReport::SPID_RELEASE_MODE);
		report.propertyValue = WriteU32LE((uint32_t)mode);

		if (!myReporter->Send(report))
			return false;
		myPad.releaseMode = mode;

		// None derives every sensor's release from its threshold, so switching to it
		// must re-apply that rule immediately. Per-sensor leaves the existing values
		// untouched.
		if (mode == RELEASE_NONE)
		{
			for (int i = 0; i < myPad.numSensors; ++i)
			{
				if (!SetSensor(i, mySensors[i]))
					return false;
			}
		}

		return true;
	}

	// Sets one sensor's threshold, release, button and gain in a single report
	// (pre-1.3 firmware: one PadConfiguration report). Each report is a HID
	// round-trip that stalls polling of every pad, so nothing is sent when the
	// device already holds these values. Live state only changes once the
	// device accepted the report.
	bool SetSensor(int idx, SensorState want)
	{
		if (!ValidSensorIndex(idx) || !ValidButton(want.button))
			return false;

		want.threshold = std::clamp(want.threshold, 0.0, 1.0);
		want.releaseThreshold = ReleaseForMode(want.threshold, want.releaseThreshold);
		want.resistorValue = std::clamp(want.resistorValue, 0, 255); // one digipot byte

		SensorState& cur = mySensors[idx];
		const auto dev = [](double v) { return ToDeviceSensorValue(v); };
		if (dev(want.threshold) == dev(cur.threshold) && dev(want.releaseThreshold) == dev(cur.releaseThreshold) &&
		    want.button == cur.button && want.resistorValue == cur.resistorValue)
			return true;

		const SensorState old = cur;
		cur.threshold = want.threshold;
		cur.releaseThreshold = want.releaseThreshold;
		cur.button = want.button;
		cur.resistorValue = want.resistorValue;

		// From v1.3 we have the SensorReport. Before that it's the PadConfiguration report
		const bool ok = myPad.firmwareVersion.IsNewer({1, 2}) ? SendSensor(idx) : SendPadConfiguration();
		if (!ok)
			cur = old;
		else if (old.button != cur.button)
			myChanges |= DCF_BUTTON_MAPPING;
		return ok;
	}

	bool SetThreshold(int sensorIndex, double threshold, double releaseThreshold)
	{
		if (!ValidSensorIndex(sensorIndex))
			return false;
		SensorState s = mySensors[sensorIndex];
		s.threshold = threshold;
		s.releaseThreshold = releaseThreshold;
		return SetSensor(sensorIndex, s);
	}

	bool SetAdcConfig(int sensorIndex, int resistorValue)
	{
		if (!ValidSensorIndex(sensorIndex))
			return false;
		SensorState s = mySensors[sensorIndex];
		s.resistorValue = resistorValue;
		return SetSensor(sensorIndex, s);
	}

	void UpdateSensor(SensorReport sensor)
	{
		if (!ValidSensorIndex(sensor.index))
		{
			return;
		}

		mySensors[sensor.index].threshold = ToNormalizedSensorValue(ReadU16LE(sensor.threshold));
		mySensors[sensor.index].releaseThreshold = ToNormalizedSensorValue(ReadU16LE(sensor.releaseThreshold));
		mySensors[sensor.index].resistorValue = sensor.resistorValue;
		mySensors[sensor.index].flags = ReadU16LE(sensor.flags);
		mySensors[sensor.index].button = (sensor.buttonMapping >= myPad.numButtons ? 0 : (sensor.buttonMapping + 1));
	}

	bool SendSensor(int sensorIndex)
	{
		if (!ValidSensorIndex(sensorIndex))
			return false;

		SensorReport report = mySensors[sensorIndex].ToReport(sensorIndex);

		bool success = myReporter->Send(report);

		if (success)
		{
			NotifyUnsavedChanges();
			UpdateSensor(report);
		}

		return success;
	}

	bool SetButtonMapping(int sensorIndex, int button)
	{
		// 0 = unmapped, else 1..numButtons; anything else would be truncated into
		// the report's int8 and shift past the 16-bit button mask in PollSensors.
		if (!ValidSensorIndex(sensorIndex))
			return false;
		SensorState s = mySensors[sensorIndex];
		s.button = button;
		return SetSensor(sensorIndex, s);
	}

	bool SendName(const char* name)
	{
		NameReport report{};
		auto length = std::strlen(name);

		// Profiles and pad state are stored under the pad name, so an empty (or
		// blank) one would put them in the parent directories.
		if (std::strspn(name, " \t") == length)
		{
			std::printf("SetName :: empty name refused\n");
			return false;
		}

		if (length > sizeof(report.name))
		{
			std::printf("SetName :: name '%s' exceeds %zi chars and was not set\n", name, sizeof(report.name));
			return false;
		}

		report.size = (uint8_t)length;
		std::memcpy(report.name, name, length);
		bool result = myReporter->SendAndGet(report);
		if (result)
		{
			NotifyUnsavedChanges();
			UpdateName(report);
		}
		return result;
	}

	bool SendLedMappingReport(const LedMappingReport& report)
	{
		if (!myReporter->Send(report))
			return false;

		UpdateLedMapping(report);

		// Only set when to update the tab
		// myChanges |= DCF_LIGHTS;

		NotifyUnsavedChanges();
		return true;
	}

	bool SendLedMapping(int ledMappingIndex, LedMapping mapping)
	{
		LedMappingReport report;
		report.ledMappingIndex = ledMappingIndex;
		report.lightRuleIndex = mapping.lightRuleIndex;
		report.flags = LMF_ENABLED;
		report.ledIndexBegin = mapping.ledIndexBegin;
		report.ledIndexEnd = mapping.ledIndexEnd;
		report.sensorIndex = mapping.sensorIndex;
		return SendLedMappingReport(report);
	}

	bool DisableLedMapping(int ledMappingIndex)
	{
		LedMappingReport report;
		report.ledMappingIndex = ledMappingIndex;
		report.lightRuleIndex = 0;
		report.flags = 0;
		report.ledIndexBegin = 0;
		report.ledIndexEnd = 0;
		report.sensorIndex = 0;
		return SendLedMappingReport(report);
	}

	bool SendLightRuleReport(const LightRuleReport& report)
	{
		if (!myReporter->Send(report))
			return false;

		UpdateLightRule(report);

		// Only set when to update the tab
		// myChanges |= DCF_LIGHTS;

		NotifyUnsavedChanges();
		return true;
	}

	bool SendLightRule(int lightRuleIndex, LightRule rule)
	{
		LightRuleReport report;
		report.lightRuleIndex = lightRuleIndex;
		report.flags = LRF_ENABLED | (LRF_FADE_ON * rule.fadeOn) | (LRF_FADE_OFF * rule.fadeOff);
		report.onColor = ToColor24(rule.onColor);
		report.offColor = ToColor24(rule.offColor);
		report.onFadeColor = ToColor24(rule.onFadeColor);
		report.offFadeColor = ToColor24(rule.offFadeColor);
		return SendLightRuleReport(report);
	}

	bool DisableLightRule(int lightRuleIndex)
	{
		LightRuleReport report;
		report.lightRuleIndex = lightRuleIndex;
		report.flags = 0;
		report.onColor = {0, 0, 0};
		report.offColor = {0, 0, 0};
		report.onFadeColor = {0, 0, 0};
		report.offFadeColor = {0, 0, 0};
		return SendLightRuleReport(report);
	}

	bool CalibrateSensor(int sensorIndex)
	{
		if (!ValidSensorIndex(sensorIndex))
			return false;

		SetPropertyReport report;
		report.propertyId = WriteU32LE(SetPropertyReport::CALIBRATE_SENSOR);
		report.propertyValue = WriteU32LE(sensorIndex);
		if (myReporter->Send(report))
			return true;
		std::printf("CalibrateSensor :: failed to send calibrate command\n");
		return false;
	}

	void Reset() { myReporter->SendReset(); }

	void FactoryReset()
	{
		// Have the device load up and save its defaults
		myReporter->SendFactoryReset();
	}

	bool SendPadConfiguration()
	{
		PadConfigurationReport report{};
		// The legacy PadConfiguration report has fixed-size arrays; never write
		// past them even if the device claims more sensors.
		int count = std::min(myPad.numSensors, SENSOR_COUNT_V1);
		for (int i = 0; i < count; ++i)
		{
			report.sensorThresholds[i] = WriteU16LE(ToDeviceSensorValue(mySensors[i].threshold));
			report.sensorToButtonMapping[i] = (mySensors[i].button == 0) ? 0xFF : (mySensors[i].button - 1);
		}
		report.releaseThreshold = WriteF32LE((float)myPad.releaseThreshold);

		bool result = myReporter->SendAndGet(report);
		if (result)
			NotifyUnsavedChanges();
		return result;
	}

	void NotifyUnsavedChanges()
	{
		myHasUnsavedChanges = true;
		myLastPendingChange = std::chrono::steady_clock::now();
	}

	bool HasUnsavedChanges() { return myHasUnsavedChanges; }

	// Stays unsaved on failure, so the next poll (2s later) retries.
	void SaveChanges()
	{
		if (myHasUnsavedChanges && myReporter->SendSaveConfiguration())
			myHasUnsavedChanges = false;
		else if (myHasUnsavedChanges)
			myLastPendingChange = std::chrono::steady_clock::now();
	}

	const DevicePath& Path() const { return myPath; }

	int PollingRate() const { return myPollingData.pollingRate; }

	const PadState& State() const { return myPad; }

	const LightsState& Lights() const { return myLights; }

	const SensorState* Sensor(int index) const
	{
		if (index < 0 || index >= myPad.numSensors)
		{
			return nullptr;
		}

		return &mySensors[index];
	}

	std::string ReadDebug()
	{
		if (!myPad.featureDebug)
		{
			return "";
		}

		DebugReport report;
		if (!myReporter->Get(report))
		{
			return "";
		}

		int messageSize = ReadU16LE(report.messageSize);

		// Bound the length by the actual packet buffer; messagePacket is not
		// guaranteed to be NUL-terminated, so never construct from a bare char*.
		messageSize = std::clamp<int>(messageSize, 0, (int)sizeof(report.messagePacket));
		if (messageSize == 0)
		{
			return "";
		}

		return std::string(report.messagePacket, (size_t)messageSize);
	}

	DeviceChanges PopChanges()
	{
		auto result = myChanges;
		myChanges = 0;
		return result;
	}

	// Apply / capture a client profile against this device. Defined out-of-line
	// in Device.cpp (they lean on json/fmt/Adp.h, kept out of this header).
	// LoadProfile returns false if any HID write failed.
	bool LoadProfile(const json& j, DeviceProfileGroups groups);
	void SaveProfile(json& j, DeviceProfileGroups groups) const;

	void TriggerChange(int type) { myChanges |= type; }

  private:
	// Shared ownership: the DeviceConnection also holds this reporter (for name
	// refreshes), so refcounting keeps it alive for whichever outlives the other.
	std::shared_ptr<Reporter> myReporter;
	DevicePath myPath;
	PadState myPad;
	LightsState myLights;
	std::vector<SensorState> mySensors;
	DeviceChanges myChanges = 0;
	bool myHasUnsavedChanges = false;
	std::chrono::time_point<std::chrono::steady_clock> myLastPendingChange;
	PollingData myPollingData;
};

} // namespace adp
