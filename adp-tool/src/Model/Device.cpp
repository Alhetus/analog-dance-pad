#include <Adp.h>

#include <atomic>
#include <memory>
#include <algorithm>
#include <map>
#include <chrono>
#include <thread>

#include "hidapi.h"
#include <fmt/format.h>

#include <Model/Device.h>
#include <Model/Reporter.h>
#include <Model/Utils.h>
#include <Model/Firmware.h>
#include <Model/Wire.h>

using namespace std;
using namespace chrono;

typedef std::function<void(uint8_t reportId, std::vector<uint8_t> data)> HidReadCallback;
int HID_API_EXPORT HID_API_CALL hid_read_register(HidReadCallback callback);

namespace adp {

struct HidIdentifier
{
	int vendorId;
	int productId;
};

constexpr HidIdentifier HID_IDS[] =
{
	// TODO: document what these correspond to.
	{0x1209, 0xb196},
	{0x03eb, 0x204f},
};

static_assert(sizeof(float) == sizeof(uint32_t), "32-bit float required");

enum LedMappingFlags
{
	LMF_ENABLED = 1 << 0,
};

enum LightRuleFlags
{
	LRF_ENABLED  = 1 << 0,
	LRF_FADE_ON  = 1 << 1,
	LRF_FADE_OFF = 1 << 2,
};


RgbColor::RgbColor(uint8_t r, uint8_t g, uint8_t b)
	: red(r), green(g), blue(b)
{
}

RgbColor::RgbColor(const std::string& input)
	: RgbColor()
{
	const char* p = input.c_str();
	if (*p == '#') ++p;

	unsigned int r = 0, g = 0, b = 0;
	if (sscanf(p, "%02x%02x%02x", &r, &g, &b) == 3)
	{
		red = (uint8_t)r;
		green = (uint8_t)g;
		blue = (uint8_t)b;
	}
	// On a malformed string the channels stay 0 (from the delegated ctor).
}

RgbColor::RgbColor()
	: red(0), green(0), blue(0)
{
}

std::string RgbColor::ToString() const
{
	return fmt::format("#{:02x}{:02x}{:02x}", red, green, blue);
}

// ====================================================================================================================
// Helper functions.
// ====================================================================================================================

static bool IsBitSet(int bits, int index)
{
	return (bits & (1 << index)) != 0;
}

// Wire-format conversion helpers (ReadU16LE/WriteU16LE/... and the sensor-value
// mapping) now live in Model/Wire.h so they can be unit-tested and reused.

static RgbColor ToRgbColor(color24 color)
{
	return { color.red, color.green, color.blue };
}

static color24 ToColor24(RgbColor color)
{
	return { color.red, color.green, color.blue };
}

SensorReport SensorState::ToReport(int index)
{
	SensorReport report;

	report.index = index;
	report.threshold = WriteU16LE(ToDeviceSensorValue(threshold));
	report.releaseThreshold = WriteU16LE(ToDeviceSensorValue(releaseThreshold));
	report.resistorValue = resistorValue;
	report.buttonMapping = button == 0 ? 0xFF : (button - 1);

	return report;
}

[[maybe_unused]] static void PrintPadConfigurationReport(const PadConfigurationReport& padConfiguration)
{
	std::printf("pad configuration [\n");
	std::printf("  releaseThreshold: %.2f\n", ReadF32LE(padConfiguration.releaseThreshold));
	std::printf("  sensors: [\n");
	for (int i = 0; i < SENSOR_COUNT_V1; ++i)
	{
		std::printf("    sensorToButtonMapping: %i\n", padConfiguration.sensorToButtonMapping[i]);
		std::printf("    sensorThresholds: %i\n", ReadU16LE(padConfiguration.sensorThresholds[i]));
	}
	std::printf("  ]\n");
	std::printf("]\n");
}

static void PrintLightRuleReport(const LightRuleReport& r)
{
	std::printf("light rule [\n");
	std::printf("  lightRuleIndex: %i\n", r.lightRuleIndex);
	std::printf("  flags: %s\n", fmt::format("{:b}", r.flags).c_str());
	std::printf("  onColor: [R%i G%i B%i]\n", r.onColor.red, r.onColor.green, r.onColor.blue);
	std::printf("  offColor: [R%i G%i B%i]\n", r.offColor.red, r.offColor.green, r.offColor.blue);
	std::printf("  onFadeColor: [R%i G%i B%i]\n", r.onFadeColor.red, r.onFadeColor.green, r.onFadeColor.blue);
	std::printf("  offFadeColor: [R%i G%i B%i]\n", r.offFadeColor.red, r.offFadeColor.green, r.offFadeColor.blue);
	std::printf("]\n");
}

static void PrintLedMappingReport(const LedMappingReport& r)
{
	std::printf("led mapping [\n");
	std::printf("  ledMappingIndex: %i\n", r.ledMappingIndex);
	std::printf("  flags: %s\n", fmt::format("{:b}", r.flags).c_str());
	std::printf("  lightRuleIndex: %i\n", r.lightRuleIndex);
	std::printf("  sensorIndex: %i\n", r.sensorIndex);
	std::printf("  ledIndexBegin: %i\n", r.ledIndexBegin);
	std::printf("  ledIndexEnd: %i\n", r.ledIndexEnd);
	std::printf("]\n");
}

static void PrintSensorReport(const SensorReport& r)
{
	std::printf("sensor config[\n");
	std::printf("  mappingIndex: %i\n", r.index);
	std::printf("  threshold: %i\n", ReadU16LE(r.threshold));
	std::printf("  releaseThreshold: %i\n", ReadU16LE(r.releaseThreshold));
	std::printf("  buttonMapping: %i\n", r.buttonMapping);
	std::printf("  resistorValue: %i\n", r.resistorValue);
	std::printf("  flags: %s\n", fmt::format("{:b}", ReadU16LE(r.flags)).c_str());
	std::printf("]\n");
}

// ====================================================================================================================
// Pad device.
// ====================================================================================================================

typedef string DevicePath;
typedef string DeviceName;

struct PollingData
{
	int readsSinceLastUpdate = 0;
	int pollingRate = 0;
	time_point<system_clock> lastUpdate;
};

class PadDevice
{
public:
	PadDevice(
		shared_ptr<Reporter> reporter,
		const char* path,
		const NameReport& name,
		const IdentificationV2Report& identification,
		const vector<LightRuleReport>& lightRules,
		const vector<LedMappingReport>& ledMappings,
		const vector<SensorReport>& sensors)
		: myReporter(std::move(reporter))
		, myPath(path)
	{
		// Clamp the device-reported sensor count so a malformed/hostile device
		// can't drive out-of-bounds access downstream.
		int sensorCount = std::clamp<int>(identification.sensorCount, 0, SENSOR_COUNT_MAX);

		mySensors = vector<SensorState>(sensorCount);

		UpdateName(name);
		myPad.maxNameLength = MAX_NAME_LENGTH;

		myPad.numButtons = identification.buttonCount;
		myPad.numSensors = sensorCount;

		// Board type is a fixed-width, not-necessarily-NUL-terminated field.
		{
			char boardType[BOARD_TYPE_LENGTH + 1] = {};
			memcpy(boardType, identification.boardType, BOARD_TYPE_LENGTH);
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

		if (myPad.firmwareVersion.IsNewer({ 1, 2 })) {
			myPad.releaseThreshold = mySensors[0].releaseThreshold / mySensors[0].threshold;
		}
		else if (myPad.firmwareVersion.IsNewer({ 1, 1 })) {
			myPad.featureLights = (bool)(identification.ledCount > 0);
		}

		try {
			SetPropertyReport report;
			report.propertyId = WriteU32LE(SetPropertyReport::SPID_SELECTED_PROPERTY);
			report.propertyValue = WriteU32LE(SetPropertyReport::SPID_RELEASE_MODE);

			if (myReporter->SendAndGet(report)) {
				int resultId = ReadU32LE(report.propertyId);
				if (resultId != SetPropertyReport::SPID_RELEASE_MODE) {
					std::printf("Fetching release mode bugged (1)\n");
				} else {
					myPad.releaseMode = (ReleaseMode)ReadU32LE(report.propertyValue);
				}
			}
		} catch(...) {
			std::printf("Fetching release mode failed\n");
		}

		UpdateLightsConfiguration(lightRules, ledMappings);
		myPollingData.lastUpdate = system_clock::now();
	}

	~PadDevice() = default;

	// True if the index is a valid position in mySensors. Used to reject
	// out-of-range indices coming from the device or from client profiles.
	bool ValidSensorIndex(int index) const
	{
		return index >= 0 && index < (int)mySensors.size();
	}

	void UpdateName(const NameReport& report)
	{
        if(report.size <= MAX_NAME_LENGTH) {
            myPad.name = "";
            myPad.name.append((const char*)report.name, (size_t)report.size);
        }
        else {
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

	void UpdateLightsConfiguration(const vector<LightRuleReport>& lightRules, const vector<LedMappingReport>& ledMappings)
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

		auto now = system_clock::now();
		if (now > myPollingData.lastUpdate + 1s)
		{	
			auto dt = duration<double>(now - myPollingData.lastUpdate).count();
			myPollingData.pollingRate = (int)lround(myPollingData.readsSinceLastUpdate / dt);
			myPollingData.readsSinceLastUpdate = 0;
			myPollingData.lastUpdate = now;
		}

		// Use the loop to save changes if needed
		if (myHasUnsavedChanges && duration_cast<std::chrono::milliseconds>(now - myLastPendingChange).count() > 2000) {
			SaveChanges();
		}

		return true;
	}

	bool SetReleaseMode(ReleaseMode mode)
	{
		myPad.releaseMode = mode;

		SetPropertyReport report;
		report.propertyId = WriteU32LE(SetPropertyReport::SPID_RELEASE_MODE);
		report.propertyValue = WriteU32LE((uint32_t)mode);
		
		return myReporter->Send(report);
	}

	bool SetThreshold(int sensorIndex, double threshold, double releaseThreshold)
	{
		if (!ValidSensorIndex(sensorIndex))
			return false;

		mySensors[sensorIndex].threshold = threshold;
		mySensors[sensorIndex].releaseThreshold = releaseThreshold;

		// From v1.3 we have the SensorReport. Before that it's the PadConfiguration report
		if (myPad.firmwareVersion.IsNewer({ 1, 2 })) {
			return SendSensor(sensorIndex);
		}
		else {
			return SendPadConfiguration();
		}
	}

	bool SetReleaseThreshold(double threshold)
	{
		myPad.releaseThreshold = clamp(threshold, 0.01, 1.00);

		// From v1.3 we have the SensorReport. Before that it's the PadConfiguration report
		if (myPad.firmwareVersion.IsNewer({ 1, 2 })) {
			for (int i = 0; i < myPad.numSensors; ++i) {
				mySensors[i].releaseThreshold = mySensors[i].threshold * myPad.releaseThreshold;
				if (!SendSensor(i)) {
					return false;
				}
			}

			return true;
		}
		else {
			return SendPadConfiguration();
		}
	}

	bool SetAdcConfig(int sensorIndex, int resistorValue)
	{
		if (!ValidSensorIndex(sensorIndex))
			return false;

		mySensors[sensorIndex].resistorValue = resistorValue;

		return SendSensor(sensorIndex);
	}

	void UpdateSensor(SensorReport sensor)
	{
		if (!ValidSensorIndex(sensor.index)) {
			return;
		}

		mySensors[sensor.index].threshold = ToNormalizedSensorValue(ReadU16LE(sensor.threshold));
		mySensors[sensor.index].releaseThreshold = ToNormalizedSensorValue(ReadU16LE(sensor.releaseThreshold));
		mySensors[sensor.index].resistorValue = sensor.resistorValue;
		mySensors[sensor.index].button = (sensor.buttonMapping >= myPad.numButtons ? 0 : (sensor.buttonMapping + 1));
	}

	bool SendSensor(int sensorIndex)
	{
		if (!ValidSensorIndex(sensorIndex))
			return false;

		SensorReport report = mySensors[sensorIndex].ToReport(sensorIndex);

		bool success = myReporter->Send(report);

		if (success) {
			NotifyUnsavedChanges();
			UpdateSensor(report);
		}

		return success;
	}

	bool SetButtonMapping(int sensorIndex, int button)
	{
		if (!ValidSensorIndex(sensorIndex))
			return false;

		mySensors[sensorIndex].button = button;
		myChanges |= DCF_BUTTON_MAPPING;

		// From v1.3 we have the SensorReport. Before that it's the PadConfiguration report
		if (myPad.firmwareVersion.IsNewer({ 1, 2 })) {
			return SendSensor(sensorIndex);
		}
		else {
			return SendPadConfiguration();
		}
	}

	bool SendName(const char* name)
	{

		NameReport report;
		auto length = strlen(name);

		if (length > sizeof(report.name))
		{
			std::printf("SetName :: name '%s' exceeds %zi chars and was not set\n", name, sizeof(report.name));
			return false;
		}

		report.size = (uint8_t)length;
		memcpy(report.name, name, length);
		bool result = myReporter->SendAndGet(report);
		if (result) {
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

	void CalibrateSensor(int sensorIndex)
	{
		if (!ValidSensorIndex(sensorIndex))
			return;

		SetPropertyReport report;
		report.propertyId = WriteU32LE(SetPropertyReport::CALIBRATE_SENSOR);
		report.propertyValue = WriteU32LE(sensorIndex);
		if (!myReporter->Send(report))
			std::printf("CalibrateSensor :: failed to send calibrate command\n");
	}

	void Reset() { myReporter->SendReset(); }

	void FactoryReset()
	{
		// Have the device load up and save its defaults
		myReporter->SendFactoryReset();
	}

	bool SendPadConfiguration()
	{
		PadConfigurationReport report;
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

		NotifyUnsavedChanges();
		return result;
	}

	void NotifyUnsavedChanges()
	{
		myHasUnsavedChanges = true;
		myLastPendingChange = system_clock::now();
	}

	bool HasUnsavedChanges()
	{
		return myHasUnsavedChanges;
	}

	void SaveChanges()
	{
		if (myHasUnsavedChanges)
		{
			myReporter->SendSaveConfiguration();
			myHasUnsavedChanges = false;
		}
	}

	const DevicePath& Path() const { return myPath; }

	const int PollingRate() const { return myPollingData.pollingRate; }

	const PadState& State() const { return myPad; }

	const LightsState& Lights() const { return myLights; }

	const SensorState* Sensor(int index)
	{
		if(index < 0 || index >= myPad.numSensors) {
			return nullptr;
		}

		return &mySensors[index];
	}

	std::string ReadDebug()
	{
		if (!myPad.featureDebug) {
			return "";
		}

		DebugReport report;
		if (!myReporter->Get(report)) {
			return "";
		}

		int messageSize = ReadU16LE(report.messageSize);

		// Bound the length by the actual packet buffer; messagePacket is not
		// guaranteed to be NUL-terminated, so never construct from a bare char*.
		messageSize = std::clamp<int>(messageSize, 0, (int)sizeof(report.messagePacket));
		if (messageSize == 0) {
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

	void TriggerChange(int type)
	{
        myChanges |= type;
	}

private:
	// Shared ownership: the DeviceConnection also holds this reporter (for name
	// refreshes), so refcounting keeps it alive for whichever outlives the other.
	shared_ptr<Reporter> myReporter;
	DevicePath myPath;
	PadState myPad;
	LightsState myLights;
	vector<SensorState> mySensors;
	DeviceChanges myChanges = 0;
	bool myHasUnsavedChanges = false;
	time_point<system_clock> myLastPendingChange;
	PollingData myPollingData;
};

// ====================================================================================================================
// Connection manager.
// ====================================================================================================================

[[maybe_unused]] static bool ContainsDevice(hid_device_info* devices, DevicePath path)
{
	for (auto device = devices; device; device = device->next)
		if (path == device->path)
			return true;

	return false;
}

enum ConnectionState
{
	CS_UNKNOWN,
	CS_PROBED,
	CS_CONNECTED,
	CS_FAILED
};

class DeviceConnection
{
public:
	DeviceConnection(std::string path):
		path(path)
	{

	}

	// The hid_device* is owned solely by the Reporter's BackendHid (which closes
	// it in its destructor), so there is no handle to close here.
	~DeviceConnection() = default;

	bool Probe()
	{
		if(state == CS_FAILED)
			return false;

		std::this_thread::sleep_for(std::chrono::milliseconds(5));

		hid_device* handle = hid_open_path(path.c_str());
		if (!handle)
		{
			std::printf("DeviceConnection :: hid_open failed (%ls) :: %s\n", hid_error(nullptr), path.c_str());
			state = CS_FAILED;
			return false;
		}

		if (hid_set_nonblocking(handle, 1) < 0)
		{
			std::printf("ConnectionManager :: hid_set_nonblocking failed\n");
			hid_close(handle);
			state = CS_FAILED;
			return false;
		}

		// Reporter/BackendHid takes ownership of the handle from here on.
		reporter = make_shared<Reporter>(handle);

		if(!reporter->Get(nameReport))
		{
			state = CS_FAILED;
			return false;
		}

		if(!reporter->Get(identificationReport))
		{
			state = CS_FAILED;
			return false;
		}

		state = CS_PROBED;
		return true;
	}

	string GetName(bool update = false)
	{
		if(update && reporter) {
			reporter->Get(nameReport);
		}

		return string((const char*)nameReport.name, nameReport.size);
	}

	string GetPath()
	{
		return path;
	}

	shared_ptr<Reporter> GetReporter()
	{
		return reporter;
	}

	ConnectionState GetState()
	{
		return state;
	}

	void SetFailed()
	{
		state = CS_FAILED;
		reporter.reset();
	}

	bool ConnectStage2();

protected:
	ConnectionState state = CS_UNKNOWN;
	std::string path;

	// Shared with the PadDevice created from this connection.
	shared_ptr<Reporter> reporter;
	NameReport nameReport;
	IdentificationV2Report identificationReport;
};

class ConnectionManager
{
public:
	~ConnectionManager()
	{
		if (myConnectedDevice)
			myConnectedDevice->SaveChanges();
	}

	PadDevice* ConnectedDevice() const { return myConnectedDevice.get(); }

	void UpdateDeviceMap(std::vector<string>& devicePaths)
	{
		for (auto id : HID_IDS)
		{
			auto foundDevices = hid_enumerate(id.vendorId, id.productId);
			for (auto dev = foundDevices; dev; dev = dev->next)
			{
				devicePaths.push_back(dev->path);
			}
			// hid_enumerate allocates a linked list that the caller must free.
			hid_free_enumeration(foundDevices);
		}
	}

	bool DiscoverDevice()
	{
		std::vector<string> devicePaths;
		UpdateDeviceMap(devicePaths);

		// Remove any devices that are no longer connected
		for (auto it = devices.begin(); it != devices.end();)
		{
			if (std::find(devicePaths.begin(), devicePaths.end(), it->first) == devicePaths.end())
			{
				if(myConnectedDevice && myConnectedDevice->Path() == it->first)
					myConnectedDevice.reset();

				std::printf("ConnectionManager :: device removed (%hs)\n", it->second.GetName().c_str());
				it = devices.erase(it);
			}
			else ++it;
		}

		for(auto& path : devicePaths)
		{
			if(devices.contains(path))
				continue;

			auto it = devices.emplace(path, path);
			if(!it.first->second.Probe())
			{
				// devices.erase(it.first);
				continue;
			}
		}

		if(!myConnectedDevice)
		{
			int c=0;
			for(auto& it : devices)
			{
				if(it.second.GetState() != CS_FAILED)
					return DeviceSelect(c);
				c++;
			}
		}

		return false;
	}

	bool ConnectToDeviceStage2(DeviceConnection& deviceCon)
	{
		shared_ptr<Reporter> reporter = deviceCon.GetReporter();
		string devicePath = deviceCon.GetPath();

		NameReport name;
		IdentificationReport padIdentification;
		IdentificationV2Report padIdentificationV2;
		vector<SensorReport> sensors;

		if (reporter == nullptr || !reporter->Get(name))
		{
			return false;
		}

		VersionType padVersion = versionTypeUnknown;

		// The other checks were fine, which means the pad doesn't support identification yet. Loading defaults.
		if (!reporter->Get(padIdentification))
		{
			padIdentification.firmwareMajor = WriteU16LE(0);
			padIdentification.firmwareMinor = WriteU16LE(0);
			padIdentification.buttonCount = MAX_BUTTON_COUNT;
			padIdentification.sensorCount = SENSOR_COUNT_V1;
			padIdentification.ledCount = 0;
			padIdentification.maxSensorValue = WriteU16LE(MAX_SENSOR_VALUE);
			memset(padIdentification.boardType, 0, BOARD_TYPE_LENGTH);
			strcpy(padIdentification.boardType, "unknown");

			memcpy(&padIdentificationV2, &padIdentification, sizeof(padIdentification));
			padIdentificationV2.features = WriteU16LE(0);
		}
		else
		{
			padVersion = { (uint16_t)ReadU16LE(padIdentification.firmwareMajor), (uint16_t)ReadU16LE(padIdentification.firmwareMinor) };

			if (padVersion.IsNewer({1, 2})) {
				if (!reporter->Get(padIdentificationV2)) {
					memcpy(&padIdentificationV2, &padIdentification, sizeof(padIdentification));
					padIdentificationV2.features = WriteU16LE(0);
				}
			}
			else {
				memcpy(&padIdentificationV2, &padIdentification, sizeof(padIdentification));
				padIdentificationV2.features = WriteU16LE(0);
			}
		}

		// If we got some lights, try to read the light rules.
		vector<LightRuleReport> lightRules;
		vector<LedMappingReport> ledMappings;
		if (padIdentification.ledCount > 0 && padVersion.IsNewer({1, 1}))
		{
			SetPropertyReport selectReport;

			LightRuleReport lightReport;
			selectReport.propertyId = WriteU32LE(SetPropertyReport::SELECTED_LIGHT_RULE_INDEX);

			for (int i = 0; i < MAX_LIGHT_RULES; ++i)
			{
				selectReport.propertyValue = WriteU32LE(i);
				if(!reporter->Send(selectReport))
					return false;

				if(!reporter->Get(lightReport))
					return false;

				if (lightReport.flags & LRF_ENABLED)
				{
					PrintLightRuleReport(lightReport);
					lightRules.push_back(lightReport);
				}
			}

			LedMappingReport ledReport;
			selectReport.propertyId = WriteU32LE(SetPropertyReport::SELECTED_LED_MAPPING_INDEX);

			for (int i = 0; i < MAX_LED_MAPPINGS; ++i)
			{
				selectReport.propertyValue = WriteU32LE(i);
				if(!reporter->Send(selectReport))
					return false;

				if(!reporter->Get(ledReport))
					return false;

				if (ledReport.flags & LMF_ENABLED)
				{
					PrintLedMappingReport(ledReport);
					ledMappings.push_back(ledReport);
				}
			}
		}

		SensorReport sensorReport;

		if (padVersion.IsNewer({ 1, 2 })) {
			SetPropertyReport selectReport;
			selectReport.propertyId = WriteU32LE(SetPropertyReport::SELECTED_SENSOR_INDEX);

			for (int i = 0; i < padIdentificationV2.sensorCount; ++i)
			{
				selectReport.propertyValue = WriteU32LE(i);
				if(!reporter->Send(selectReport))
					return false;

				if(!reporter->Get(sensorReport))
					return false;

				PrintSensorReport(sensorReport);
				sensors.push_back(sensorReport);
			}
		}
		else {
			// Backwards compat
			PadConfigurationReport padConfig;
			if (reporter->Get(padConfig)) {
				for (int i = 0; i < SENSOR_COUNT_V1; ++i)
				{
					sensorReport.index = i;
					sensorReport.threshold = padConfig.sensorThresholds[i];
					auto th = ReadU16LE(padConfig.sensorThresholds[i]);
					auto rt = ReadF32LE(padConfig.releaseThreshold);
					sensorReport.releaseThreshold = WriteU16LE(int(th * rt));
					sensorReport.buttonMapping = padConfig.sensorToButtonMapping[i];
					sensorReport.resistorValue = 0;
					sensorReport.flags = WriteU16LE(0);

					sensors.push_back(sensorReport);
				}
			}
		}

		auto device = make_unique<PadDevice>(
			reporter,
			devicePath.c_str(),
			name,
			padIdentificationV2,
			lightRules,
			ledMappings,
			sensors);

		std::string boardType = device->State().boardType.ToString();

		std::printf("ConnectionManager :: new device connected [\n");
		std::printf("  Name: %s\n", device->State().name.c_str());
		std::printf("  Board: %s: %s\n", padIdentificationV2.boardType, boardType.c_str());
		std::printf("  Firmware version: v%u.%u\n", ReadU16LE(padIdentificationV2.firmwareMajor), ReadU16LE(padIdentificationV2.firmwareMinor));
		std::printf("  Feature flags: %s\n", fmt::format("{:b}", ReadU16LE(padIdentificationV2.features)).c_str());
		std::printf("  Path: %s\n", devicePath.c_str());

		std::printf("]\n");

		myConnectedDevice = std::move(device);
		return true;
	}

	void DisconnectFailedDevice()
	{
		auto device = myConnectedDevice.get();
		if (device)
		{
			if(devices.contains(device->Path()))
				devices.at(device->Path()).SetFailed();

			myConnectedDevice.reset();
		}
	}

	void AddIncompatibleDevice(hid_device_info* device)
	{
		if (device->product_string) // Can be null on failure, apparently.
			myFailedDevices[device->path] = narrow(device->product_string, wcslen(device->product_string));
	}

	int DeviceNumber()
	{
		return (int)devices.size();
	}

	string GetDeviceName(int index, bool update = false)
	{
		if (index < 0 || index >= devices.size())
			return "";

		auto it = devices.begin();
		std::advance(it, index);
		return it->second.GetName(update);
	}

	bool DeviceSelect(int index)
	{
		if (index < 0 || index >= devices.size())
			return false;

		if(index == DeviceSelected())
			return true;

		auto it = devices.begin();
		std::advance(it, index);

		if(!it->second.ConnectStage2())
			return false;

		return true;
	}

	int DeviceSelected()
	{
		if(!myConnectedDevice)
			return -1;

		int c = 0;
		
		for(auto& it : devices)
		{
			if(myConnectedDevice->Path() == it.first)
				return c;
			c++;
		}

		return -1;
	}

	bool ConnectToUrl(string url)
	{
		if (devices.contains(url)) {
			// Already known: find its index and select it.
			int c = 0;
			for (auto& it : devices)
			{
				if (it.second.GetPath() == url)
					return DeviceSelect(c);
				++c;
			}

			return false;
		}

		auto it = devices.emplace(url, url);
		if (!it.first->second.Probe())
		{
			devices.erase(it.first);
			return false;
		}

		if(!it.first->second.ConnectStage2())
		{
			devices.erase(it.first);
			return false;
		}

		return true;
	}

private:
	map<DevicePath, DeviceConnection> devices;
	
	unique_ptr<PadDevice> myConnectedDevice;
	map<DevicePath, DeviceName> myFailedDevices;
	bool emulator = false;
};

// ====================================================================================================================
// Device API.
// ====================================================================================================================

static std::unique_ptr<ConnectionManager> connectionManager;
static std::atomic<bool> searching = true;

// The latest immutable sensor snapshot. Written only on the device-I/O thread
// (PublishSnapshot); read lock-free from any thread (GetSnapshot).
static std::atomic<std::shared_ptr<const SensorSnapshot>> gSnapshot;


bool DeviceConnection::ConnectStage2()
{
	if(!connectionManager)
		return false;

	if(state == CS_FAILED)
		return false;

	for(int tries=0; tries<3; ++tries)
	{
		if(connectionManager->ConnectToDeviceStage2(*this))
			return true;

		std::printf("DeviceConnection :: ConnectStage2 failed (%d)\n", tries);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	state = CS_FAILED;
	return false;
}


void Device::Init()
{
	hid_init();

	connectionManager = std::make_unique<ConnectionManager>();
}

void Device::Shutdown()
{
	connectionManager.reset();

	gSnapshot.store(nullptr);

	hid_exit();
}

DeviceChanges Device::Update()
{
	DeviceChanges changes = 0;

	// If there is currently no connected device, try to find one.
	auto device = connectionManager->ConnectedDevice();
	if (!device && searching)
	{
		if (connectionManager->DiscoverDevice())
			changes |= DCF_DEVICE;

		device = connectionManager->ConnectedDevice();
	}

	// If there is a device, update it. All hidapi access happens here on the
	// single device-I/O thread.
	if (device)
	{
		changes |= device->PopChanges();

		if (!device->PollSensors())
		{
			connectionManager->DisconnectFailedDevice();
			changes |= DCF_DEVICE;
		}

		if(changes & DCF_NAME) {
			connectionManager->GetDeviceName(connectionManager->DeviceSelected(), true);
		}
	}

	// Publish the latest immutable snapshot for the WebSocket side to read.
	PublishSnapshot();

	return changes;
}

int Device::PollingRate()
{
	auto device = connectionManager->ConnectedDevice();
	return device ? device->PollingRate() : 0;
}

const PadState* Device::Pad()
{
	auto device = connectionManager->ConnectedDevice();
	return device ? &device->State() : nullptr;
}

const LightsState* Device::Lights()
{
	auto device = connectionManager->ConnectedDevice();
	return device ? &device->Lights() : nullptr;
}

const SensorState* Device::Sensor(int sensorIndex)
{
	auto device = connectionManager->ConnectedDevice();
	return device ? device->Sensor(sensorIndex) : nullptr;
}

void Device::PublishSnapshot()
{
	auto snapshot = std::make_shared<SensorSnapshot>();

	auto device = connectionManager ? connectionManager->ConnectedDevice() : nullptr;
	snapshot->deviceCount = connectionManager ? connectionManager->DeviceNumber() : 0;

	if (device)
	{
		const PadState& pad = device->State();
		snapshot->connected = true;
		snapshot->name = pad.name;
		snapshot->pollingRate = device->PollingRate();
		snapshot->releaseThreshold = pad.releaseThreshold;
		snapshot->releaseMode = (int)pad.releaseMode;

		snapshot->sensors.reserve(pad.numSensors);
		for (int i = 0; i < pad.numSensors; ++i)
		{
			if (const SensorState* s = device->Sensor(i))
				snapshot->sensors.push_back(*s);
		}
	}

	// Publish as immutable; readers get a stable, consistent view.
	gSnapshot.store(std::shared_ptr<const SensorSnapshot>(std::move(snapshot)));
}

std::shared_ptr<const SensorSnapshot> Device::GetSnapshot()
{
	return gSnapshot.load();
}

void Device::SnapshotToJson(const SensorSnapshot& snapshot, json& j)
{
	j["msgType"] = 1;
	j["deviceIndex"] = snapshot.deviceCount;
	j["name"] = snapshot.name;
	j["pollingRate"] = snapshot.pollingRate;
	j["releaseThreshold"] = snapshot.releaseThreshold;
	j["releaseMode"] = snapshot.releaseMode;
	j["sensors"] = json::array();

	for (const SensorState& s : snapshot.sensors)
	{
		json sensor;
		sensor["threshold"] = s.threshold;
		sensor["releaseThreshold"] = s.releaseThreshold;
		sensor["value"] = s.value;
		sensor["resistorValue"] = s.resistorValue;
		sensor["button"] = s.button;
		sensor["pressed"] = s.pressed;
		j["sensors"].push_back(std::move(sensor));
	}
}

void Device::HandleClientMessage(const std::string& message)
{
	// Runs on the device-I/O thread, so any device mutation triggered here is
	// single-threaded and safe. Parsing is fully defensive: a malformed or
	// unrecognized message is logged and ignored, never fatal.
	json j;
	try
	{
		j = json::parse(message);
	}
	catch (const std::exception& e)
	{
		std::printf("HandleClientMessage :: invalid JSON ignored (%s)\n", e.what());
		return;
	}

	if (!j.is_object())
	{
		std::printf("HandleClientMessage :: non-object message ignored\n");
		return;
	}

	// Only act when a device is connected; otherwise there is nothing to apply.
	if (!connectionManager || !connectionManager->ConnectedDevice())
	{
		std::printf("HandleClientMessage :: no device connected, message ignored\n");
		return;
	}

	// A configuration update reuses the hardened LoadProfile path, which
	// validates every index and group before touching device state.
	if (j.contains("sensors") || j.contains("releaseThreshold") || j.contains("name") || j.contains("lightRules"))
	{
		try
		{
			LoadProfile(j, DGP_ALL);
		}
		catch (const std::exception& e)
		{
			std::printf("HandleClientMessage :: failed to apply config (%s)\n", e.what());
		}
		return;
	}

	std::printf("HandleClientMessage :: unhandled message: %s\n", message.c_str());
}

std::string Device::ReadDebug()
{
	auto device = connectionManager->ConnectedDevice();
	return device ? device->ReadDebug() : "";
}

const bool Device::HasUnsavedChanges()
{
	auto device = connectionManager->ConnectedDevice();
	return device ? device->HasUnsavedChanges() : false;
}

bool Device::SetThreshold(int sensorIndex, double threshold, double releaseThreshold)
{
	auto device = connectionManager->ConnectedDevice();
	return device ? device->SetThreshold(sensorIndex, threshold, releaseThreshold) : false;
}

bool Device::SetReleaseThreshold(double threshold)
{
	auto device = connectionManager->ConnectedDevice();
	return device ? device->SetReleaseThreshold(threshold) : false;
}

bool Device::SetAdcConfig(int sensorIndex, int resistorValue)
{
	auto device = connectionManager->ConnectedDevice();
	return device ? device->SetAdcConfig(sensorIndex, resistorValue) : false;
}

bool Device::SetButtonMapping(int sensorIndex, int button)
{
	auto device = connectionManager->ConnectedDevice();
	return device ? device->SetButtonMapping(sensorIndex, button) : false;
}

bool Device::SetDeviceName(const char* name)
{
	auto device = connectionManager->ConnectedDevice();
	return device ? device->SendName(name) : false;
}

bool Device::SendLedMapping(int ledMappingIndex, LedMapping mapping)
{
	auto device = connectionManager->ConnectedDevice();
	return device ? device->SendLedMapping(ledMappingIndex, mapping) : false;
}

bool Device::DisableLedMapping(int ledMappingIndex)
{
	auto device = connectionManager->ConnectedDevice();
	return device ? device->DisableLedMapping(ledMappingIndex) : false;
}

bool Device::SendLightRule(int lightRuleIndex, LightRule rule)
{
	auto device = connectionManager->ConnectedDevice();
	return device ? device->SendLightRule(lightRuleIndex, rule) : false;
}

bool Device::DisableLightRule(int lightRuleIndex)
{
	auto device = connectionManager->ConnectedDevice();
	return device ? device->DisableLightRule(lightRuleIndex) : false;
}

void Device::CalibrateSensor(int sensorIndex)
{
	auto device = connectionManager->ConnectedDevice();
	if(device) {
		device->CalibrateSensor(sensorIndex);
	}
}

bool Device::SetReleaseMode(ReleaseMode mode)
{
	auto device = connectionManager->ConnectedDevice();

	if (device) {
		return device->SetReleaseMode(mode);
	}

	return false;
}

void Device::SendDeviceReset()
{
	auto device = connectionManager->ConnectedDevice();
	if (device) device->Reset();
}

void Device::SendFactoryReset()
{
	auto device = connectionManager->ConnectedDevice();
	if (device) device->FactoryReset();
}

void Device::SaveChanges()
{
	auto device = connectionManager->ConnectedDevice();
	if (device) device->SaveChanges();
}

void Device::SetSearching(bool s)
{
	searching = s;
}

void Device::DiscoverNewDevices()
{
	connectionManager->DiscoverDevice();
}

int Device::DeviceNumber()
{
	if(!connectionManager) return 0;
	return connectionManager->DeviceNumber();
}

string Device::GetDeviceName(int index)
{
	if(!connectionManager) return "";
	return connectionManager->GetDeviceName(index);
}

bool Device::DeviceSelect(int index)
{
	if(!connectionManager) return false;
	return connectionManager->DeviceSelect(index);
}

int Device::DeviceSelected()
{
	if(!connectionManager) return -1;
	return connectionManager->DeviceSelected();
}


void Device::LoadProfile(json& j, DeviceProfileGroups groups)
{
	const PadState* pad = Pad();
	if (!pad) {
		std::printf("LoadProfile :: no device connected\n");
		return;
	}

	if ((groups & DPG_LIGHTS) && pad->featureLights) {
		if (j.contains("ledMappings") && j["ledMappings"].is_array()) {
			const auto& ledMappings = j["ledMappings"];
			for (size_t key = 0; key < ledMappings.size() && key < (size_t)MAX_LED_MAPPINGS; ++key) {
				const auto& value = ledMappings[key];

				LedMapping lm = {
					value.value("lightRuleIndex", 0),
					value.value("sensorIndex", 0),
					value.value("ledIndexBegin", 0),
					value.value("ledIndexEnd", 0)
				};

				SendLedMapping((int)key, lm);
			}

			// Disable any slots not present in the profile.
			for (int i = (int)std::min<size_t>(ledMappings.size(), MAX_LED_MAPPINGS); i < MAX_LED_MAPPINGS; ++i) {
				DisableLedMapping(i);
			}
		}

		if (j.contains("lightRules") && j["lightRules"].is_array()) {
			const auto& lightRules = j["lightRules"];
			for (size_t key = 0; key < lightRules.size() && key < (size_t)MAX_LIGHT_RULES; ++key) {
				const auto& value = lightRules[key];

				auto color = [&value](const char* name) {
					return (value.contains(name) && value[name].is_string())
						? RgbColor((string)value[name]) : RgbColor(0, 0, 0);
				};

				LightRule lr = {
					value.value("fadeOn", false),
					value.value("fadeOff", false),
					color("onColor"),
					color("offColor"),
					color("onFadeColor"),
					color("offFadeColor")
				};

				SendLightRule((int)key, lr);
			}

			for (int i = (int)std::min<size_t>(lightRules.size(), MAX_LIGHT_RULES); i < MAX_LIGHT_RULES; ++i) {
				DisableLightRule(i);
			}
		}

		auto device = connectionManager ? connectionManager->ConnectedDevice() : nullptr;
		if (device) {
			device->TriggerChange(DCF_LIGHTS);
		}
	}

	if (j.contains("sensors") && j["sensors"].is_array()) {
		const auto& sensors = j["sensors"];
		for (size_t key = 0; key < sensors.size(); ++key) {
			const auto& sensor = sensors[key];
			const int idx = (int)key;

			if ((groups & DPG_SENSITIVITY) && sensor.contains("threshold") && sensor["threshold"].is_number()) {
				double th = sensor["threshold"].get<double>();
				SetThreshold(idx, th, th);
			}

			if ((groups & DPG_MAPPING) && sensor.contains("button") && sensor["button"].is_number_integer()) {
				SetButtonMapping(idx, sensor["button"].get<int>());
			}

			if ((groups & DPG_MAPPING) && sensor.contains("resistorValue") && sensor["resistorValue"].is_number_integer() && pad->featureDigipot) {
				SetAdcConfig(idx, sensor["resistorValue"].get<int>());
			}
		}
	}

	if (groups & DPG_SENSITIVITY) {
		if (j.contains("releaseThreshold") && j["releaseThreshold"].is_number()) {
			SetReleaseThreshold(j["releaseThreshold"].get<double>());
		}
	}

	if (groups & DPG_DEVICE) {
		if (j.contains("name") && j["name"].is_string()) {
			SetDeviceName(j["name"].get<string>().c_str());
		}
	}
}

void Device::SaveProfile(json& j, DeviceProfileGroups groups)
{
	const PadState* pad = Pad();
	if (!pad) {
		std::printf("SaveProfile :: no device connected\n");
		return;
	}

	j["adpToolVersion"] = fmt::format("v{}.{}", ADP_VERSION_MAJOR, ADP_VERSION_MINOR);

	if ((groups & DPG_LIGHTS) && pad->featureLights) {
		if (const LightsState* lights = Lights()) {
			j["ledMappings"] = json::array();
			for (const auto& [index, lm] : lights->ledMappings) {
				j["ledMappings"][index]["lightRuleIndex"] = lm.lightRuleIndex;
				j["ledMappings"][index]["sensorIndex"] = lm.sensorIndex;
				j["ledMappings"][index]["ledIndexBegin"] = lm.ledIndexBegin;
				j["ledMappings"][index]["ledIndexEnd"] = lm.ledIndexEnd;
			}

			j["lightRules"] = json::array();
			for (const auto& [index, lr] : lights->lightRules) {
				j["lightRules"][index]["fadeOn"] = lr.fadeOn;
				j["lightRules"][index]["fadeOff"] = lr.fadeOff;
				j["lightRules"][index]["onColor"] = lr.onColor.ToString();
				j["lightRules"][index]["offColor"] = lr.offColor.ToString();
				j["lightRules"][index]["onFadeColor"] = lr.onFadeColor.ToString();
				j["lightRules"][index]["offFadeColor"] = lr.offFadeColor.ToString();
			}
		}
	}

	if (groups & (DPG_SENSITIVITY | DPG_MAPPING)) {
		j["sensors"] = json::array();
		for (int i = 0; i < pad->numSensors; ++i)
		{
			const SensorState* s = Sensor(i);
			if (!s)
				continue;

			if (groups & DPG_SENSITIVITY) {
				j["sensors"][i]["threshold"] = s->threshold;
				j["sensors"][i]["releaseThreshold"] = s->releaseThreshold;
			}

			if (groups & DPG_MAPPING) {
				j["sensors"][i]["button"] = s->button;
				j["sensors"][i]["resistorValue"] = s->resistorValue;
			}
		}

		j["releaseThreshold"] = pad->releaseThreshold;
	}

	if (groups & DPG_DEVICE) {
		j["name"] = pad->name;
	}
}

}
