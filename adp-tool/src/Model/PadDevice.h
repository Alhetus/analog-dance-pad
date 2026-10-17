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
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Model/Device.h>
#include <Model/Profiles.h>
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
	LRF_KNOWN = LRF_ENABLED | LRF_FADE_ON | LRF_FADE_OFF,
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

		myPad.numButtons = identification.buttonCount;
		myPad.numSensors = sensorCount;
		myPad.ledCount = identification.ledCount;
		myPad.maxSensorValue = SensorScale(ReadU16LE(identification.maxSensorValue));

		// Board type is a fixed-width, not-necessarily-NUL-terminated field.
		const char* board = identification.boardType;
		myPad.boardType.assign(board, std::find(board, board + BOARD_TYPE_LENGTH, '\0'));

		myPad.firmwareVersion.major = ReadU16LE(identification.firmwareMajor);
		myPad.firmwareVersion.minor = ReadU16LE(identification.firmwareMinor);

		uint16_t features = ReadU16LE(identification.features);
		myPad.featureDigipot = (features & IdentificationV2Report::FEATURE_DIGIPOT) != 0;
		myPad.featureLights = (features & IdentificationV2Report::FEATURE_LIGHTS) != 0;

		for (auto sensor : sensors)
		{
			UpdateSensor(sensor);
		}

		SetPropertyReport report;
		report.propertyId = WriteU32LE(SetPropertyReport::SPID_SELECTED_PROPERTY);
		report.propertyValue = WriteU32LE(SetPropertyReport::SPID_RELEASE_MODE);
		if (!myReporter->SendAndGet(report))
			std::printf("Fetching release mode failed\n");
		else if (ReadU32LE(report.propertyId) != SetPropertyReport::SPID_RELEASE_MODE)
			std::printf("Fetching release mode bugged (1)\n");
		else
		{
			// Only None stays None. Global release mode has been removed: a device
			// still reporting it (or any unknown value) is treated as per-sensor,
			// and its stored per-sensor release values carry over unchanged.
			myPad.releaseMode = ReadU32LE(report.propertyValue) == RELEASE_NONE ? RELEASE_NONE : RELEASE_INDIVIDUAL;
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
			// Raw device bytes: anything but UTF-8 would make every json dump of
			// the name throw.
			myPad.name = Profiles::ValidUtf8(std::string((const char*)report.name, (size_t)report.size));
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
			myRuleExtraFlags[report.lightRuleIndex] = report.flags & ~LRF_KNOWN;
		}
		else
		{
			myLights.lightRules.erase(report.lightRuleIndex);
			myRuleExtraFlags.erase(report.lightRuleIndex);
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
				{
					const int v = ReadU16LE(report.sensorValues[i]);
					aggregateValues[i] += v;
					// Past the reported scale every value shows as 1.0 (and no threshold can reach it).
					if (v > myPad.maxSensorValue && !myWarnedOverScale)
					{
						std::printf(
						    "PadDevice :: '%s' sensor %d reads %d, above its maxSensorValue %d: values saturate\n",
						    myPad.name.c_str(), i, v, myPad.maxSensorValue);
						myWarnedOverScale = true;
					}
				}
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
				mySensors[i].value = ToNormalizedSensorValue(value, myPad.maxSensorValue);
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

		return true;
	}

	// Edits are saved to the pad's EEPROM 2s after the last one (StartSave: a
	// save can block for a second or more).
	bool SaveDue() const
	{
		return myHasUnsavedChanges && !Saving() &&
		       std::chrono::steady_clock::now() - myLastPendingChange > std::chrono::seconds(2);
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
		// Nor near 0: the pad releases only once the value drops to it, which ADC
		// noise can keep from ever happening (a stuck button).
		return std::clamp(requested, std::min(MinReleaseThreshold(), threshold), threshold);
	}

	// MIN_RELEASE_COUNTS, normalized for this pad.
	double MinReleaseThreshold() const { return (double)MIN_RELEASE_COUNTS / myPad.maxSensorValue; }

	// True if the firmware could light this mapping: an existing sensor and rule
	// slot and an LED range on the pad. It skips anything else silently, so such
	// a mapping is refused instead of being shown as set. (It also skips one
	// whose rule is disabled: see UnlitMapping.)
	bool ValidLedMapping(const LedMapping& m) const
	{
		return ValidSensorIndex(m.sensorIndex) && m.lightRuleIndex < MAX_LIGHT_RULES &&
		       m.ledIndexBegin <= m.ledIndexEnd && m.ledIndexEnd <= myPad.ledCount;
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
		// The firmware keeps it in RAM until the next save-configuration.
		NotifyUnsavedChanges();

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

	// Sets one sensor's threshold, release, button and gain in a single report.
	// Each report is a HID
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
		const auto dev = [&](double v) { return ToDeviceSensorValue(v, myPad.maxSensorValue); };
		if (dev(want.threshold) == dev(cur.threshold) && dev(want.releaseThreshold) == dev(cur.releaseThreshold) &&
		    want.button == cur.button && want.resistorValue == cur.resistorValue)
			return true;

		const SensorState old = cur;
		cur.threshold = want.threshold;
		cur.releaseThreshold = want.releaseThreshold;
		cur.button = want.button;
		cur.resistorValue = want.resistorValue;

		const bool ok = SendSensor(idx);
		if (!ok)
			cur = old;
		return ok;
	}

	void UpdateSensor(SensorReport sensor)
	{
		if (!ValidSensorIndex(sensor.index))
		{
			return;
		}

		const int max = myPad.maxSensorValue;
		mySensors[sensor.index].threshold = ToNormalizedSensorValue(ReadU16LE(sensor.threshold), max);
		mySensors[sensor.index].releaseThreshold = ToNormalizedSensorValue(ReadU16LE(sensor.releaseThreshold), max);
		mySensors[sensor.index].resistorValue = sensor.resistorValue;
		mySensors[sensor.index].flags = ReadU16LE(sensor.flags);
		// Negative (0xFF is the firmware's "unmapped") or past the last button: unmapped.
		const int mapping = sensor.buttonMapping;
		mySensors[sensor.index].button = mapping < 0 || mapping >= myPad.numButtons ? 0 : mapping + 1;
	}

	bool SendSensor(int sensorIndex)
	{
		if (!ValidSensorIndex(sensorIndex))
			return false;

		SensorReport report = mySensors[sensorIndex].ToReport(sensorIndex, myPad.maxSensorValue);

		bool success = myReporter->Send(report);

		if (success)
		{
			NotifyUnsavedChanges();
			UpdateSensor(report);
		}

		return success;
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
		NotifyUnsavedChanges();
		return true;
	}

	bool SendLightRule(int lightRuleIndex, LightRule rule)
	{
		LightRuleReport report;
		report.lightRuleIndex = lightRuleIndex;
		// Flag bits this server doesn't know (e.g. the firmware's LRF_PULSE) are kept.
		const auto extra = myRuleExtraFlags.find(lightRuleIndex);
		report.flags = LRF_ENABLED | (LRF_FADE_ON * rule.fadeOn) | (LRF_FADE_OFF * rule.fadeOff) |
		               (extra == myRuleExtraFlags.end() ? 0 : extra->second);
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

	void NotifyUnsavedChanges()
	{
		myHasUnsavedChanges = true;
		myLastPendingChange = std::chrono::steady_clock::now();
	}

	bool HasUnsavedChanges() const { return myHasUnsavedChanges; }

	// True while the last two save attempts failed (it is retried every 2s).
	bool SaveFailed() const { return mySaveFailures >= 2; }

	// Starts saving the unsaved edits on a worker thread, so a long EEPROM write
	// (the AVR firmware writes it inside the request, interrupts off, ~3.4ms
	// per changed byte) stalls only this pad. Until FinishSave collects it the
	// worker owns the HID handle: nothing else may touch this pad's device
	// (Saving() pads are skipped and their messages held).
	void StartSave()
	{
		if (!myHasUnsavedChanges || Saving())
			return;
		// A raw pointer: the reporter (and hid_close) must not die on the worker.
		// mySave is declared after myReporter, so it is waited for first.
		mySave = std::async(std::launch::async, [r = myReporter.get()] { return r->SendSaveConfiguration(); });
	}

	bool Saving() const { return mySave.valid(); }

	// Collects a finished save; with `wait`, waits for a running one first.
	void FinishSave(bool wait = false)
	{
		if (!mySave.valid() || (!wait && mySave.wait_for(std::chrono::seconds(0)) != std::future_status::ready))
			return;
		SaveResult(mySave.get());
	}

	// Saves on this thread. Stays unsaved on failure, so it is retried 2s
	// later. True if nothing is left unsaved.
	bool SaveChanges()
	{
		FinishSave(true);
		if (myHasUnsavedChanges)
			SaveResult(myReporter->SendSaveConfiguration());
		return !myHasUnsavedChanges;
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

	DeviceChanges PopChanges()
	{
		auto result = myChanges;
		myChanges = 0;
		return result;
	}

	// Apply / capture a client profile against this device. Defined out-of-line
	// in Device.cpp (they lean on json/fmt/Adp.h, kept out of this header).
	// LoadProfile returns false if any setting was refused or a HID write
	// failed, adding what didn't apply (e.g. "sensor 3") to `failed`.
	bool LoadProfile(const json& j, DeviceProfileGroups groups, std::vector<std::string>* failed = nullptr);
	void SaveProfile(json& j, DeviceProfileGroups groups) const;

	// The lights arrays of a configure or profile, parsed slot by slot (an
	// absent array stays empty). False, with nothing parsed and the reason in
	// `why`, if any slot is malformed or a mapping is outside this pad's
	// sensors, rules or LEDs.
	bool ParseLights(const json& j, std::vector<std::optional<LedMapping>>& mappings,
	                 std::vector<std::optional<LightRule>>& rules, std::string* why = nullptr) const;

	// Why a client's lights edit would set an LED mapping the firmware never
	// lights (its rule is disabled), or "". Mappings the pad already holds are
	// let through. Not checked on profile loads: they restore what a pad had.
	std::string UnlitMapping(const json& j) const;

	void TriggerChange(int type) { myChanges |= type; }

  private:
	// A single failure isn't reported: the firmware writes the EEPROM inside
	// the request, so a big save can outlast hidapi's write timeout and still
	// succeed; the retry then only rewrites what differs.
	void SaveResult(bool ok)
	{
		if (!ok)
		{
			++mySaveFailures;
			myLastPendingChange = std::chrono::steady_clock::now();
			return;
		}
		mySaveFailures = 0;
		myHasUnsavedChanges = false;
	}

	// Shared ownership: the DeviceConnection also holds this reporter (for name
	// refreshes), so refcounting keeps it alive for whichever outlives the other.
	std::shared_ptr<Reporter> myReporter;
	DevicePath myPath;
	PadState myPad;
	LightsState myLights;
	std::vector<SensorState> mySensors;
	DeviceChanges myChanges = 0;
	bool myHasUnsavedChanges = false;
	int mySaveFailures = 0; // consecutive
	std::chrono::time_point<std::chrono::steady_clock> myLastPendingChange;
	PollingData myPollingData;
	std::future<bool> mySave;                // a running StartSave
	std::map<int, uint8_t> myRuleExtraFlags; // per light rule: flag bits this server doesn't know
	bool myWarnedOverScale = false;
};

} // namespace adp
