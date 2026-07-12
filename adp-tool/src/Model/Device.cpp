#include <Adp.h>

#include <atomic>
#include <mutex>
#include <memory>
#include <algorithm>
#include <map>
#include <chrono>
#include <thread>
#include <ctime>
#include <cstdint>
#include <filesystem>

#include "hidapi.h"
#include <fmt/format.h>

#include <Model/Device.h>
#include <Model/PadDevice.h>
#include <Model/Reporter.h>
#include <Model/Utils.h>
#include <Model/Firmware.h>
#include <Model/Wire.h>
#include <Model/Profiles.h>

using namespace std;
using namespace chrono;

typedef std::function<void(uint8_t reportId, std::vector<uint8_t> data)> HidReadCallback;
int HID_API_EXPORT HID_API_CALL hid_read_register(HidReadCallback callback);

namespace adp
{

struct HidIdentifier
{
	int vendorId;
	int productId;
};

constexpr HidIdentifier HID_IDS[] = {
    // TODO: document what these correspond to.
    {0x1209, 0xb196},
    {0x03eb, 0x204f},
};

static_assert(sizeof(float) == sizeof(uint32_t), "32-bit float required");

RgbColor::RgbColor(uint8_t r, uint8_t g, uint8_t b) : red(r), green(g), blue(b)
{
}

RgbColor::RgbColor(const std::string& input) : RgbColor()
{
	const char* p = input.c_str();
	if (*p == '#')
		++p;

	unsigned int r = 0, g = 0, b = 0;
	if (sscanf(p, "%02x%02x%02x", &r, &g, &b) == 3)
	{
		red = (uint8_t)r;
		green = (uint8_t)g;
		blue = (uint8_t)b;
	}
	// On a malformed string the channels stay 0 (from the delegated ctor).
}

RgbColor::RgbColor() : red(0), green(0), blue(0)
{
}

std::string RgbColor::ToString() const
{
	return fmt::format("#{:02x}{:02x}{:02x}", red, green, blue);
}

// ====================================================================================================================
// Helper functions.
// ====================================================================================================================

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

typedef string DeviceName;

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
	DeviceConnection(std::string path) : path(path) {}

	// The hid_device* is owned solely by the Reporter's BackendHid (which closes
	// it in its destructor), so there is no handle to close here.
	~DeviceConnection() = default;

	bool Probe()
	{
		if (state == CS_FAILED)
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

		if (!reporter->Get(nameReport))
		{
			state = CS_FAILED;
			return false;
		}

		if (!reporter->Get(identificationReport))
		{
			state = CS_FAILED;
			return false;
		}

		state = CS_PROBED;
		return true;
	}

	string GetName(bool update = false)
	{
		if (update && reporter)
		{
			reporter->Get(nameReport);
		}

		return string((const char*)nameReport.name, nameReport.size);
	}

	string GetPath() { return path; }

	shared_ptr<Reporter> GetReporter() { return reporter; }

	ConnectionState GetState() { return state; }

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
				if (myConnectedDevice && myConnectedDevice->Path() == it->first)
					myConnectedDevice.reset();

				std::printf("ConnectionManager :: device removed (%s)\n", it->second.GetName().c_str());
				it = devices.erase(it);
			}
			else
				++it;
		}

		for (auto& path : devicePaths)
		{
			if (devices.contains(path))
				continue;

			auto it = devices.emplace(path, path);
			if (!it.first->second.Probe())
			{
				// devices.erase(it.first);
				continue;
			}
		}

		if (!myConnectedDevice)
		{
			int c = 0;
			for (auto& it : devices)
			{
				if (it.second.GetState() != CS_FAILED)
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

			static_cast<IdentificationReport&>(padIdentificationV2) = padIdentification;
			padIdentificationV2.features = WriteU16LE(0);
		}
		else
		{
			padVersion = {(uint16_t)ReadU16LE(padIdentification.firmwareMajor),
			              (uint16_t)ReadU16LE(padIdentification.firmwareMinor)};

			if (padVersion.IsNewer({1, 2}))
			{
				if (!reporter->Get(padIdentificationV2))
				{
					static_cast<IdentificationReport&>(padIdentificationV2) = padIdentification;
					padIdentificationV2.features = WriteU16LE(0);
				}
			}
			else
			{
				static_cast<IdentificationReport&>(padIdentificationV2) = padIdentification;
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
				if (!reporter->Send(selectReport))
					return false;

				if (!reporter->Get(lightReport))
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
				if (!reporter->Send(selectReport))
					return false;

				if (!reporter->Get(ledReport))
					return false;

				if (ledReport.flags & LMF_ENABLED)
				{
					PrintLedMappingReport(ledReport);
					ledMappings.push_back(ledReport);
				}
			}
		}

		SensorReport sensorReport;

		if (padVersion.IsNewer({1, 2}))
		{
			SetPropertyReport selectReport;
			selectReport.propertyId = WriteU32LE(SetPropertyReport::SELECTED_SENSOR_INDEX);

			for (int i = 0; i < padIdentificationV2.sensorCount; ++i)
			{
				selectReport.propertyValue = WriteU32LE(i);
				if (!reporter->Send(selectReport))
					return false;

				if (!reporter->Get(sensorReport))
					return false;

				PrintSensorReport(sensorReport);
				sensors.push_back(sensorReport);
			}
		}
		else
		{
			// Backwards compat
			PadConfigurationReport padConfig;
			if (reporter->Get(padConfig))
			{
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

		auto device = make_unique<PadDevice>(reporter, devicePath.c_str(), name, padIdentificationV2, lightRules,
		                                     ledMappings, sensors);

		std::string boardType = device->State().boardType.ToString();

		std::printf("ConnectionManager :: new device connected [\n");
		std::printf("  Name: %s\n", device->State().name.c_str());
		std::printf("  Board: %s: %s\n", padIdentificationV2.boardType, boardType.c_str());
		std::printf("  Firmware version: v%u.%u\n", ReadU16LE(padIdentificationV2.firmwareMajor),
		            ReadU16LE(padIdentificationV2.firmwareMinor));
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
			if (devices.contains(device->Path()))
				devices.at(device->Path()).SetFailed();

			myConnectedDevice.reset();
		}
	}

	void AddIncompatibleDevice(hid_device_info* device)
	{
		if (device->product_string) // Can be null on failure, apparently.
			myFailedDevices[device->path] = narrow(device->product_string, wcslen(device->product_string));
	}

	int DeviceNumber() { return (int)devices.size(); }

	string GetDeviceName(int index, bool update = false)
	{
		if (index < 0 || (size_t)index >= devices.size())
			return "";

		auto it = devices.begin();
		std::advance(it, index);
		return it->second.GetName(update);
	}

	string GetDevicePath(int index)
	{
		if (index < 0 || (size_t)index >= devices.size())
			return "";

		auto it = devices.begin();
		std::advance(it, index);
		return it->first;
	}

	bool DeviceSelect(int index)
	{
		if (index < 0 || (size_t)index >= devices.size())
			return false;

		if (index == DeviceSelected())
			return true;

		auto it = devices.begin();
		std::advance(it, index);

		if (!it->second.ConnectStage2())
			return false;

		return true;
	}

	int DeviceSelected()
	{
		if (!myConnectedDevice)
			return -1;

		int c = 0;

		for (auto& it : devices)
		{
			if (myConnectedDevice->Path() == it.first)
				return c;
			c++;
		}

		return -1;
	}

	bool ConnectToUrl(string url)
	{
		if (devices.contains(url))
		{
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

		if (!it.first->second.ConnectStage2())
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
};

// ====================================================================================================================
// Device API.
// ====================================================================================================================

static std::unique_ptr<ConnectionManager> connectionManager;
static std::atomic<bool> searching = true;

// Profile storage. gProfilesDir is set once at startup (before threads), then
// only read. gProfilesDirty starts true so the first tick broadcasts the list.
static std::string gProfilesDir = "profiles";
static std::atomic<bool> gProfilesDirty{true};

// Set when the connected pad's lights change (or a device is (re)selected), so
// the WebSocket loop can broadcast the lights message only when it matters
// instead of on every ~60Hz snapshot.
static std::atomic<bool> gLightsDirty{false};

// Profiles capture thresholds+release, per-sensor gain, and lights — but NOT
// button mappings or the device name (those are pad-specific, not preferences).
static constexpr DeviceProfileGroups PROFILE_GROUPS =
    (DeviceProfileGroups)(DPG_SENSITIVITY | DPG_GAIN | DPG_LIGHTS);

// The latest immutable sensor snapshot. Written only on the device-I/O thread
// (PublishSnapshot); read from any thread (GetSnapshot). Guarded by a mutex
// rather than std::atomic<shared_ptr> because Apple's libc++ doesn't implement
// the C++20 atomic<shared_ptr> specialization. The critical section is a single
// pointer swap/copy, so contention is negligible.
static std::mutex gSnapshotMutex;
static std::shared_ptr<const SensorSnapshot> gSnapshot;

bool DeviceConnection::ConnectStage2()
{
	if (!connectionManager)
		return false;

	if (state == CS_FAILED)
		return false;

	for (int tries = 0; tries < 3; ++tries)
	{
		if (connectionManager->ConnectToDeviceStage2(*this))
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

	{
		std::lock_guard<std::mutex> lock(gSnapshotMutex);
		gSnapshot = nullptr;
	}

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

		if (changes & DCF_NAME)
		{
			connectionManager->GetDeviceName(connectionManager->DeviceSelected(), true);
		}
	}

	// A lights change (or a fresh device) means the gated lights message is stale.
	if (changes & (DCF_LIGHTS | DCF_DEVICE))
		gLightsDirty = true;

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
	snapshot->selectedIndex = connectionManager ? connectionManager->DeviceSelected() : -1;

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

		if (pad.featureLights)
			snapshot->lights = device->Lights();
	}

	// Publish as immutable; readers get a stable, consistent view.
	std::lock_guard<std::mutex> lock(gSnapshotMutex);
	gSnapshot = std::move(snapshot);
}

std::shared_ptr<const SensorSnapshot> Device::GetSnapshot()
{
	std::lock_guard<std::mutex> lock(gSnapshotMutex);
	return gSnapshot;
}

// Serializes lights (rules + LED mappings) to the wire shape shared by the
// profile format (SaveProfile) and the lights message, so a stored profile and a
// live pad can be compared field-for-field on the client for drift detection.
static void AppendLightsToJson(const LightsState& lights, json& j)
{
	j["ledMappings"] = json::array();
	for (const auto& [index, lm] : lights.ledMappings)
	{
		j["ledMappings"][index]["lightRuleIndex"] = lm.lightRuleIndex;
		j["ledMappings"][index]["sensorIndex"] = lm.sensorIndex;
		j["ledMappings"][index]["ledIndexBegin"] = lm.ledIndexBegin;
		j["ledMappings"][index]["ledIndexEnd"] = lm.ledIndexEnd;
	}

	j["lightRules"] = json::array();
	for (const auto& [index, lr] : lights.lightRules)
	{
		j["lightRules"][index]["fadeOn"] = lr.fadeOn;
		j["lightRules"][index]["fadeOff"] = lr.fadeOff;
		j["lightRules"][index]["onColor"] = lr.onColor.ToString();
		j["lightRules"][index]["offColor"] = lr.offColor.ToString();
		j["lightRules"][index]["onFadeColor"] = lr.onFadeColor.ToString();
		j["lightRules"][index]["offFadeColor"] = lr.offFadeColor.ToString();
	}
}

void Device::SnapshotToJson(const SensorSnapshot& snapshot, json& j)
{
	j["msgType"] = 1;
	j["deviceIndex"] = snapshot.deviceCount;
	j["selectedIndex"] = snapshot.selectedIndex;
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

// Lights change rarely and are ~1KB, so they travel as their own message
// (msgType 4) that the WebSocket loop broadcasts only on change / periodically —
// not with every 60Hz snapshot. Same shape a saved profile uses, so the client
// can drift-check a loaded profile's lights field-for-field.
void Device::LightsToJson(const SensorSnapshot& snapshot, json& j)
{
	j["msgType"] = 4;
	AppendLightsToJson(snapshot.lights, j);
}

bool Device::TakeLightsDirty()
{
	return gLightsDirty.exchange(false);
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

	// Device selection works even with no device currently connected (it is how
	// a client connects to one), so handle it before the connected-device guard.
	if (j.contains("selectDevice") && j["selectDevice"].is_number_integer())
	{
		if (connectionManager)
			connectionManager->DeviceSelect(j["selectDevice"].get<int>());
		gLightsDirty = true; // resend lights promptly for the newly selected pad
		return;
	}

	// Profile browsing/editing/deleting is filesystem-only (no device needed), so
	// handle it before the connected-device guard. Profiles::* validate the id.
	if (j.contains("listProfiles"))
	{
		gProfilesDirty = true; // triggers a fresh broadcast on the next tick
		return;
	}

	if (j.contains("deleteProfile") && j["deleteProfile"].is_string())
	{
		if (Profiles::Delete(gProfilesDir, j["deleteProfile"].get<std::string>()))
			gProfilesDirty = true;
		return;
	}

	// Edit metadata (name/description) only — never re-captures the pad.
	if (j.contains("updateProfile") && j["updateProfile"].is_object())
	{
		const auto& u = j["updateProfile"];
		json blob;
		if (u.contains("id") && u["id"].is_string() && Profiles::Read(gProfilesDir, u["id"].get<std::string>(), blob))
		{
			if (u.contains("name") && u["name"].is_string())
				blob["name"] = u["name"].get<std::string>();
			if (u.contains("description") && u["description"].is_string())
				blob["description"] = u["description"].get<std::string>();
			if (Profiles::Write(gProfilesDir, blob.value("id", std::string()), blob))
				gProfilesDirty = true;
		}
		return;
	}

	// Only act when a device is connected; otherwise there is nothing to apply.
	if (!connectionManager || !connectionManager->ConnectedDevice())
	{
		std::printf("HandleClientMessage :: no device connected, message ignored\n");
		return;
	}

	// Save the connected pad's current settings as a named profile. Two forms:
	//   {saveProfile:{name}}      -> create a new profile (slug id, de-collided)
	//   {saveProfile:{id}}        -> overwrite an existing one, keeping its name
	if (j.contains("saveProfile") && j["saveProfile"].is_object())
	{
		const auto& sp = j["saveProfile"];
		std::string id, name, description;

		if (sp.contains("id") && sp["id"].is_string())
		{
			json existing;
			if (!Profiles::Read(gProfilesDir, sp["id"].get<std::string>(), existing))
			{
				std::printf("saveProfile :: unknown id, ignored\n");
				return;
			}
			id = existing.value("id", std::string());
			name = existing.value("name", std::string());
			description = existing.value("description", std::string());
		}
		else if (sp.contains("name") && sp["name"].is_string())
		{
			name = sp["name"].get<std::string>();
			description = sp.value("description", std::string());
			id = Profiles::SlugifyId(name);
			std::string base = id;
			for (int n = 2; Profiles::Exists(gProfilesDir, id); ++n)
				id = base + "-" + std::to_string(n);
		}
		else
		{
			std::printf("saveProfile :: needs a name or id, ignored\n");
			return;
		}

		json blob;
		SaveProfile(blob, PROFILE_GROUPS); // capture the connected device
		blob["id"] = id;
		blob["name"] = name;
		blob["description"] = description;
		blob["sensorCount"] = Pad() ? Pad()->numSensors : 0;
		blob["savedAt"] = (int64_t)std::time(nullptr);
		if (Profiles::Write(gProfilesDir, id, blob))
			gProfilesDirty = true;
		return;
	}

	// Apply a stored profile to the connected pad. Refuse a sensor-count mismatch
	// so a 4-panel profile is never squeezed onto a differently-shaped pad.
	if (j.contains("loadProfile") && j["loadProfile"].is_string())
	{
		json blob;
		if (!Profiles::Read(gProfilesDir, j["loadProfile"].get<std::string>(), blob))
		{
			std::printf("loadProfile :: unknown id, ignored\n");
			return;
		}
		const int numSensors = Pad() ? Pad()->numSensors : 0;
		if (blob.value("sensorCount", -1) != numSensors)
		{
			std::printf("loadProfile :: sensor-count mismatch, ignored\n");
			return;
		}
		try
		{
			LoadProfile(blob, PROFILE_GROUPS);
		}
		catch (const std::exception& e)
		{
			std::printf("loadProfile :: failed (%s)\n", e.what());
		}
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

bool Device::HasUnsavedChanges()
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
	if (device)
	{
		device->CalibrateSensor(sensorIndex);
	}
}

bool Device::SetReleaseMode(ReleaseMode mode)
{
	auto device = connectionManager->ConnectedDevice();

	if (device)
	{
		return device->SetReleaseMode(mode);
	}

	return false;
}

void Device::SendDeviceReset()
{
	auto device = connectionManager->ConnectedDevice();
	if (device)
		device->Reset();
}

void Device::SendFactoryReset()
{
	auto device = connectionManager->ConnectedDevice();
	if (device)
		device->FactoryReset();
}

void Device::SaveChanges()
{
	auto device = connectionManager->ConnectedDevice();
	if (device)
		device->SaveChanges();
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
	if (!connectionManager)
		return 0;
	return connectionManager->DeviceNumber();
}

string Device::GetDeviceName(int index)
{
	if (!connectionManager)
		return "";
	return connectionManager->GetDeviceName(index);
}

string Device::GetDevicePath(int index)
{
	if (!connectionManager)
		return "";
	return connectionManager->GetDevicePath(index);
}

void Device::DeviceListToJson(json& j)
{
	j["msgType"] = 2;
	j["selectedIndex"] = connectionManager ? connectionManager->DeviceSelected() : -1;
	j["devices"] = json::array();

	int count = connectionManager ? connectionManager->DeviceNumber() : 0;
	for (int i = 0; i < count; ++i)
	{
		json device;
		device["index"] = i;
		device["id"] = connectionManager->GetDevicePath(i);
		device["name"] = connectionManager->GetDeviceName(i);
		j["devices"].push_back(std::move(device));
	}
}

void Device::SetProfilesDir(const std::string& dir)
{
	gProfilesDir = dir.empty() ? "profiles" : dir;
	std::error_code ec;
	std::filesystem::create_directories(gProfilesDir, ec);
	if (ec)
		std::printf("SetProfilesDir :: could not create '%s' (%s)\n", gProfilesDir.c_str(), ec.message().c_str());
	else
		std::printf("Profiles directory: %s\n", gProfilesDir.c_str());
	gProfilesDirty = true;
}

void Device::ProfileListToJson(json& j)
{
	j["msgType"] = 3;
	j["profiles"] = Profiles::List(gProfilesDir);
}

bool Device::TakeProfilesDirty()
{
	return gProfilesDirty.exchange(false);
}

bool Device::DeviceSelect(int index)
{
	if (!connectionManager)
		return false;
	return connectionManager->DeviceSelect(index);
}

int Device::DeviceSelected()
{
	if (!connectionManager)
		return -1;
	return connectionManager->DeviceSelected();
}

void PadDevice::LoadProfile(json& j, DeviceProfileGroups groups)
{
	const PadState* pad = &myPad;

	if ((groups & DPG_LIGHTS) && pad->featureLights)
	{
		if (j.contains("ledMappings") && j["ledMappings"].is_array())
		{
			const auto& ledMappings = j["ledMappings"];
			for (size_t key = 0; key < ledMappings.size() && key < (size_t)MAX_LED_MAPPINGS; ++key)
			{
				const auto& value = ledMappings[key];

				LedMapping lm = {value.value("lightRuleIndex", 0), value.value("sensorIndex", 0),
				                 value.value("ledIndexBegin", 0), value.value("ledIndexEnd", 0)};

				SendLedMapping((int)key, lm);
			}

			// Disable any slots not present in the profile.
			for (int i = (int)std::min<size_t>(ledMappings.size(), MAX_LED_MAPPINGS); i < MAX_LED_MAPPINGS; ++i)
			{
				DisableLedMapping(i);
			}
		}

		if (j.contains("lightRules") && j["lightRules"].is_array())
		{
			const auto& lightRules = j["lightRules"];
			for (size_t key = 0; key < lightRules.size() && key < (size_t)MAX_LIGHT_RULES; ++key)
			{
				const auto& value = lightRules[key];

				auto color = [&value](const char* name) {
					return (value.contains(name) && value[name].is_string()) ? RgbColor((string)value[name])
					                                                         : RgbColor(0, 0, 0);
				};

				LightRule lr = {
				    value.value("fadeOn", false), value.value("fadeOff", false), color("onColor"), color("offColor"),
				    color("onFadeColor"),         color("offFadeColor")};

				SendLightRule((int)key, lr);
			}

			for (int i = (int)std::min<size_t>(lightRules.size(), MAX_LIGHT_RULES); i < MAX_LIGHT_RULES; ++i)
			{
				DisableLightRule(i);
			}
		}

		TriggerChange(DCF_LIGHTS);
	}

	if (j.contains("sensors") && j["sensors"].is_array())
	{
		const auto& sensors = j["sensors"];
		for (size_t key = 0; key < sensors.size(); ++key)
		{
			const auto& sensor = sensors[key];
			const int idx = (int)key;

			if ((groups & DPG_SENSITIVITY) && sensor.contains("threshold") && sensor["threshold"].is_number())
			{
				double th = sensor["threshold"].get<double>();
				// Restore the stored per-sensor release faithfully; fall back to
				// the threshold only when a profile predates the release field.
				double rel = (sensor.contains("releaseThreshold") && sensor["releaseThreshold"].is_number())
				                 ? sensor["releaseThreshold"].get<double>()
				                 : th;
				SetThreshold(idx, th, rel);
			}

			if ((groups & DPG_MAPPING) && sensor.contains("button") && sensor["button"].is_number_integer())
			{
				SetButtonMapping(idx, sensor["button"].get<int>());
			}

			if ((groups & DPG_GAIN) && sensor.contains("resistorValue") &&
			    sensor["resistorValue"].is_number_integer() && pad->featureDigipot)
			{
				SetAdcConfig(idx, sensor["resistorValue"].get<int>());
			}
		}
	}

	if (groups & DPG_SENSITIVITY)
	{
		if (j.contains("releaseThreshold") && j["releaseThreshold"].is_number())
		{
			SetReleaseThreshold(j["releaseThreshold"].get<double>());
		}
	}

	if (groups & DPG_DEVICE)
	{
		if (j.contains("name") && j["name"].is_string())
		{
			SendName(j["name"].get<string>().c_str());
		}
	}
}

void PadDevice::SaveProfile(json& j, DeviceProfileGroups groups)
{
	const PadState* pad = &myPad;

	j["adpToolVersion"] = fmt::format("v{}.{}", ADP_VERSION_MAJOR, ADP_VERSION_MINOR);

	if ((groups & DPG_LIGHTS) && pad->featureLights)
	{
		AppendLightsToJson(Lights(), j);
	}

	if (groups & (DPG_SENSITIVITY | DPG_MAPPING | DPG_GAIN))
	{
		j["sensors"] = json::array();
		for (int i = 0; i < pad->numSensors; ++i)
		{
			const SensorState* s = Sensor(i);
			if (!s)
				continue;

			if (groups & DPG_SENSITIVITY)
			{
				j["sensors"][i]["threshold"] = s->threshold;
				j["sensors"][i]["releaseThreshold"] = s->releaseThreshold;
			}

			if (groups & DPG_MAPPING)
			{
				j["sensors"][i]["button"] = s->button;
			}

			if (groups & DPG_GAIN)
			{
				j["sensors"][i]["resistorValue"] = s->resistorValue;
			}
		}

		j["releaseThreshold"] = pad->releaseThreshold;
	}

	if (groups & DPG_DEVICE)
	{
		j["name"] = pad->name;
	}
}

void Device::LoadProfile(json& j, DeviceProfileGroups groups)
{
	auto device = connectionManager ? connectionManager->ConnectedDevice() : nullptr;
	if (!device)
	{
		std::printf("LoadProfile :: no device connected\n");
		return;
	}
	device->LoadProfile(j, groups);
}

void Device::SaveProfile(json& j, DeviceProfileGroups groups)
{
	auto device = connectionManager ? connectionManager->ConnectedDevice() : nullptr;
	if (!device)
	{
		std::printf("SaveProfile :: no device connected\n");
		return;
	}
	device->SaveProfile(j, groups);
}

} // namespace adp
