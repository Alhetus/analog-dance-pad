#pragma once

#include "stdint.h"
#include <string>
#include <map>
#include <memory>
#include <vector>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include <Model/Firmware.h>
#include <Model/Reporter.h>

namespace adp
{

enum DeviceChangeFlags
{
	DCF_DEVICE = 1 << 0,
	DCF_BUTTON_MAPPING = 1 << 1,
	DCF_NAME = 1 << 2,
	DCF_LIGHTS = 1 << 3
};

typedef int32_t DeviceChanges;

enum DeviceProfileGroupFlags
{
	DPG_SENSITIVITY = 1 << 0,
	DPG_MAPPING = 1 << 1,
	DPG_DEVICE = 1 << 2,
	DPG_LIGHTS = 1 << 3,

	DGP_ALL = 0b1111111111111111
};

typedef int32_t DeviceProfileGroups;

struct RgbColor
{
	RgbColor(uint8_t r, uint8_t g, uint8_t b);
	RgbColor(const std::string& input);
	RgbColor();

	std::string ToString() const;

	uint8_t red;
	uint8_t green;
	uint8_t blue;
};

struct SensorState
{
	double threshold = 0.0;
	double releaseThreshold = 0.0;
	double value = 0.0;
	int resistorValue = 0;
	int button = 0; // zero means unmapped.
	bool pressed = false;

	SensorReport ToReport(int index);
};

// An immutable, self-contained copy of everything the WebSocket clients need to
// see about the currently connected pad. Built on the device-I/O thread each
// tick and published via an atomic shared_ptr, so readers never touch live
// device state (no locks, no torn reads, no TOCTOU null-deref).
struct SensorSnapshot
{
	bool connected = false;
	int deviceCount = 0;
	int selectedIndex = -1; // index of the streamed device in the discovered-device list, or -1.
	std::string name;
	int pollingRate = 0;
	double releaseThreshold = 1.0;
	int releaseMode = 0;
	std::vector<SensorState> sensors;
};

struct VersionType
{
	uint16_t major;
	uint16_t minor;

	bool IsNewer(VersionType then)
	{
		if (major > then.major)
		{
			return true;
		}

		if (major == then.major && minor > then.minor)
		{
			return true;
		}

		return false;
	}
};

static const VersionType versionTypeUnknown = {0, 0};

struct PadState
{
	std::string name;
	int maxNameLength = 0;
	int numButtons = 0;
	int numSensors = 0;
	double releaseThreshold = 1.0;
	BoardTypeStruct boardType;
	bool featureDebug;
	bool featureDigipot;
	bool featureLights;
	VersionType firmwareVersion = versionTypeUnknown;
	ReleaseMode releaseMode = ReleaseMode::RELEASE_GLOBAL;
};

struct LedMapping
{
	int lightRuleIndex;
	int sensorIndex;
	int ledIndexBegin;
	int ledIndexEnd;
};

struct LightRule
{
	bool fadeOn;
	bool fadeOff;
	RgbColor onColor;
	RgbColor offColor;
	RgbColor onFadeColor;
	RgbColor offFadeColor;
};

struct LightsState
{
	std::map<int, LightRule> lightRules;
	std::map<int, LedMapping> ledMappings;
};

class Device
{
  public:
	static void Init();

	static void Shutdown();

	static DeviceChanges Update();

	static int PollingRate();

	static const PadState* Pad();

	static const LightsState* Lights();

	static const SensorState* Sensor(int sensorIndex);

	// Builds an immutable snapshot of the connected pad and publishes it
	// atomically. Called on the device-I/O thread (from Update()).
	static void PublishSnapshot();

	// Returns the most recently published snapshot (may be null before the
	// first tick). Lock-free; safe to call from any thread.
	static std::shared_ptr<const SensorSnapshot> GetSnapshot();

	// Serializes a snapshot to the client-facing JSON wire format.
	static void SnapshotToJson(const SensorSnapshot& snapshot, json& j);

	// Serializes the list of discovered devices (msgType 2) so clients can
	// enumerate pads and pick which one to stream/control.
	static void DeviceListToJson(json& j);

	// Parses and dispatches one inbound client message. Must be called on the
	// device-I/O thread so device access stays single-threaded.
	static void HandleClientMessage(const std::string& message);

	static std::string ReadDebug();

	static bool HasUnsavedChanges();

	static bool SetReleaseMode(ReleaseMode mode);

	static bool SetThreshold(int sensorIndex, double threshold, double releaseThreshold);

	static bool SetAdcConfig(int sensorIndex, int resistorValue);

	static bool SetReleaseThreshold(double threshold);

	static bool SetButtonMapping(int sensorIndex, int button);

	static bool SetDeviceName(const char* name);

	static bool SendLedMapping(int ledMappingIndex, LedMapping mapping);

	static bool DisableLedMapping(int ledMappingIndex);

	static bool SendLightRule(int lightRuleIndex, LightRule rule);

	static bool DisableLightRule(int lightRuleIndex);

	static void CalibrateSensor(int sensorIndex);

	static void SendDeviceReset();

	static void SendFactoryReset();

	static void SaveChanges();

	static int DeviceNumber();

	static std::string GetDeviceName(int index);

	static std::string GetDevicePath(int index);

	static bool DeviceSelect(int index);

	static int DeviceSelected();

	static void LoadProfile(json& j, DeviceProfileGroups groups);

	static void SaveProfile(json& j, DeviceProfileGroups groups);

	static void SetSearching(bool s);

	static void DiscoverNewDevices();
};

} // namespace adp
