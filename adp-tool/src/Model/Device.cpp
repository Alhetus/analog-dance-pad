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
#include <Model/Players.h>
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
		for (auto& [path, pad] : myPads)
			pad->SaveChanges();
	}

	// The pad the static Device::* API currently acts on. Set per pad by
	// Update() while polling and by HandleClientMessage from the message's `pad`.
	// ponytail: a current-pad pointer instead of threading a PadDevice& through
	// the whole static API; safe because only the device-I/O thread touches it.
	PadDevice* ConnectedDevice() const { return myCurrent; }

	void SetCurrent(PadDevice* pad) { myCurrent = pad; }

	PadDevice* Find(const string& path) const
	{
		auto it = myPads.find(path);
		return it == myPads.end() ? nullptr : it->second.get();
	}

	vector<PadDevice*> Pads() const
	{
		vector<PadDevice*> out;
		for (auto& [path, pad] : myPads)
			out.push_back(pad.get());
		return out;
	}

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
				DropPad(it->first);

				std::printf("ConnectionManager :: device removed (%s)\n", it->second.GetName().c_str());
				it = devices.erase(it);
			}
			else
				++it;
		}

		// Probe and connect every newly seen pad; all pads are polled every tick.
		bool connected = false;
		for (auto& path : devicePaths)
		{
			if (devices.contains(path))
				continue;

			auto it = devices.emplace(path, path);
			if (it.first->second.Probe())
				connected |= it.first->second.ConnectStage2();
		}

		return connected;
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

		myPads[devicePath] = std::move(device);
		return true;
	}

	void DisconnectFailedDevice(const string& path)
	{
		if (devices.contains(path))
			devices.at(path).SetFailed();
		DropPad(path);
	}

	void DropPad(const string& path)
	{
		auto it = myPads.find(path);
		if (it == myPads.end())
			return;
		if (myCurrent == it->second.get())
			myCurrent = nullptr;
		myPads.erase(it);
	}

	// Refreshes the cached name of a pad's connection (after a rename).
	void RefreshName(const string& path)
	{
		if (devices.contains(path))
			devices.at(path).GetName(true);
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

  private:
	map<DevicePath, DeviceConnection> devices;

	map<DevicePath, unique_ptr<PadDevice>> myPads;
	PadDevice* myCurrent = nullptr;
	map<DevicePath, DeviceName> myFailedDevices;
};

// ====================================================================================================================
// Device API.
// ====================================================================================================================

static std::unique_ptr<ConnectionManager> connectionManager;
static std::atomic<bool> searching = true;

// Root of the local profile store (see Profiles.h for the layout). Set once at
// startup (before threads), then only read.
static Profiles::Path gProfilesDir = "profiles";

// Set when a pad's lights change (or pads come and go), so the WebSocket loop
// broadcasts the lights messages only when it matters instead of every tick.
static std::atomic<bool> gLightsDirty{false};

// Server-owned per-pad selection {player, profileId, profileName, recent[]},
// keyed by pad name and persisted at <profilesDir>/pads/<pad>/state.json so it
// survives restarts. Device-I/O thread only.
static std::map<std::string, json> gPadStates;
static std::atomic<bool> gPadStatesDirty{true};

// Profiles capture thresholds+release, per-sensor gain, and lights — but NOT
// button mappings or the device name (those are pad-specific, not preferences).
static constexpr DeviceProfileGroups PROFILE_GROUPS = (DeviceProfileGroups)(DPG_SENSITIVITY | DPG_GAIN | DPG_LIGHTS);

// The latest immutable snapshots, one per pad. Written only on the device-I/O
// thread (PublishSnapshot); read from any thread (GetSnapshots). Guarded by a
// mutex rather than std::atomic<shared_ptr> because Apple's libc++ doesn't
// implement the C++20 atomic<shared_ptr> specialization. The critical section
// is a single pointer swap/copy, so contention is negligible.
static std::mutex gSnapshotMutex;
static std::shared_ptr<const std::vector<SensorSnapshot>> gSnapshots;

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
		gSnapshots = nullptr;
	}

	hid_exit();
}

DeviceChanges Device::Update()
{
	DeviceChanges changes = 0;

	// With no pads at all, look for some right away (DiscoverNewDevices also
	// runs ~1Hz to pick up hot-plugged pads).
	if (connectionManager->Pads().empty() && searching && connectionManager->DiscoverDevice())
		changes |= DCF_DEVICE;

	// Poll every pad. All hidapi access happens here on the device-I/O thread.
	for (PadDevice* device : connectionManager->Pads())
	{
		connectionManager->SetCurrent(device);
		const string path = device->Path(); // the device is gone after a disconnect
		DeviceChanges padChanges = device->PopChanges();

		if (!device->PollSensors())
		{
			connectionManager->DisconnectFailedDevice(path);
			changes |= DCF_DEVICE;
			continue;
		}

		if (padChanges & DCF_NAME)
			connectionManager->RefreshName(path);
		changes |= padChanges;
	}
	connectionManager->SetCurrent(nullptr);

	// A lights change (or pads coming/going) means the gated messages are stale.
	if (changes & (DCF_LIGHTS | DCF_DEVICE))
		gLightsDirty = true;
	if (changes & (DCF_DEVICE | DCF_NAME))
		gPadStatesDirty = true;

	// Publish the latest immutable snapshots for the WebSocket side to read.
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
	auto snapshots = std::make_shared<std::vector<SensorSnapshot>>();

	for (PadDevice* device : connectionManager ? connectionManager->Pads() : vector<PadDevice*>{})
	{
		SensorSnapshot snapshot;
		const PadState& pad = device->State();
		snapshot.connected = true;
		snapshot.path = device->Path();
		snapshot.name = pad.name;
		snapshot.pollingRate = device->PollingRate();
		snapshot.releaseThreshold = pad.releaseThreshold;
		snapshot.releaseMode = (int)pad.releaseMode;
		snapshot.numButtons = pad.numButtons;
		snapshot.featureDigipot = pad.featureDigipot;

		snapshot.sensors.reserve(pad.numSensors);
		for (int i = 0; i < pad.numSensors; ++i)
		{
			if (const SensorState* s = device->Sensor(i))
				snapshot.sensors.push_back(*s);
		}

		if (pad.featureLights)
			snapshot.lights = device->Lights();

		snapshots->push_back(std::move(snapshot));
	}

	// Publish as immutable; readers get a stable, consistent view.
	std::lock_guard<std::mutex> lock(gSnapshotMutex);
	gSnapshots = std::move(snapshots);
}

std::shared_ptr<const std::vector<SensorSnapshot>> Device::GetSnapshots()
{
	std::lock_guard<std::mutex> lock(gSnapshotMutex);
	return gSnapshots;
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
	j["pad"] = snapshot.path;
	j["name"] = snapshot.name;
	j["pollingRate"] = snapshot.pollingRate;
	j["releaseThreshold"] = snapshot.releaseThreshold;
	j["releaseMode"] = snapshot.releaseMode;
	j["numButtons"] = snapshot.numButtons;
	j["featureDigipot"] = snapshot.featureDigipot;
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
	j["pad"] = snapshot.path;
	AppendLightsToJson(snapshot.lights, j);
}

bool Device::TakeLightsDirty()
{
	return gLightsDirty.exchange(false);
}

// ---- Pad state ---------------------------------------------------------------------------------------------------

static Profiles::Path PadStateDir(const std::string& pad)
{
	return gProfilesDir / "pads" / Profiles::U8Path(Profiles::EscapeName(pad));
}

// The cached state for a pad, loaded from disk on first use.
static json& PadStateFor(const std::string& pad)
{
	auto it = gPadStates.find(pad);
	if (it != gPadStates.end())
		return it->second;
	json st;
	if (!Profiles::Read(PadStateDir(pad), "state", st) || !st.is_object())
		st = json::object();
	return gPadStates[pad] = st;
}

static void SavePadState(const std::string& pad)
{
	if (!Profiles::Write(PadStateDir(pad), "state", PadStateFor(pad)))
		std::printf("SavePadState :: could not write state for '%s'\n", pad.c_str());
	gPadStatesDirty = true;
}

// Marks `player`/`id` as the pad's current selection and moves the player to
// the front of the pad's recent list.
static void SetPadSelection(const std::string& pad, const std::string& player, const std::string& id,
                            const std::string& profileName)
{
	json& st = PadStateFor(pad);
	st["player"] = player;
	st["profileId"] = id;
	st["profileName"] = profileName;
	json recent = json::array({player});
	for (const auto& r : st.value("recent", json::array()))
		if (r.is_string() && r != player && recent.size() < 8)
			recent.push_back(r);
	st["recent"] = recent;
	SavePadState(pad);
}

void Device::PadStatesToJson(json& j)
{
	j["msgType"] = 5;
	j["pads"] = json::object();
	for (PadDevice* device : connectionManager ? connectionManager->Pads() : vector<PadDevice*>{})
	{
		json st = PadStateFor(device->State().name);
		st["name"] = device->State().name;
		j["pads"][device->Path()] = st;
	}
}

bool Device::TakePadStatesDirty()
{
	return gPadStatesDirty.exchange(false);
}

// A pad was renamed: move its profile dirs (every player) and its state along.
// ponytail: walks all player dirs on the device thread; renaming is a rare operator action.
static void RenamePad(const std::string& from, const std::string& to)
{
	if (from == to || from.empty() || to.empty())
		return;
	std::error_code ec;
	const auto escFrom = Profiles::U8Path(Profiles::EscapeName(from));
	const auto escTo = Profiles::U8Path(Profiles::EscapeName(to));
	for (const auto& player : std::filesystem::directory_iterator(gProfilesDir / "players", ec))
	{
		if (!std::filesystem::exists(player.path() / escTo, ec))
			std::filesystem::rename(player.path() / escFrom, player.path() / escTo, ec);
	}
	if (!std::filesystem::exists(PadStateDir(to), ec))
		std::filesystem::rename(PadStateDir(from), PadStateDir(to), ec);
	gPadStates.erase(from);
	gPadStates.erase(to);
	gPadStatesDirty = true;
	Players::RequestRescan();
	std::printf("Pad renamed '%s' -> '%s', profiles moved\n", from.c_str(), to.c_str());
}

// ---- Client messages ---------------------------------------------------------------------------------------------

// All of one player's stored profiles, grouped by pad name (msgType 6).
static json PlayerProfilesJson(const std::string& player)
{
	json j = {{"msgType", 6}, {"player", player}, {"pads", json::object()}};
	const auto dir = gProfilesDir / "players" / Profiles::U8Path(Profiles::EscapeName(player));
	std::error_code ec;
	for (const auto& pad : std::filesystem::directory_iterator(dir, ec))
	{
		if (!pad.is_directory(ec))
			continue;
		json list = Profiles::List(pad.path());
		if (!list.empty())
			j["pads"][Profiles::UnescapeName(Profiles::PathU8(pad.path().filename()))] = std::move(list);
	}
	return j;
}

// A new id for `name` that is free in `dir`.
static std::string NewProfileId(const Profiles::Path& dir, const std::string& name)
{
	const std::string base = Profiles::SlugifyId(name);
	std::string id = base;
	for (int n = 2; Profiles::Exists(dir, id); ++n)
		id = base + "-" + std::to_string(n);
	return id;
}

// The `player` field of a profile op, validated against the scanned player
// list. That check is the trust boundary: only known names reach a path.
static bool ValidPlayer(const json& op, std::string& player)
{
	if (!op.is_object() || !op.contains("player") || !op["player"].is_string())
		return false;
	player = op["player"].get<std::string>();
	if (Players::Has(player))
		return true;
	std::printf("HandleClientMessage :: unknown player '%s', ignored\n", player.c_str());
	return false;
}

static bool HasString(const json& op, const char* key)
{
	return op.contains(key) && op[key].is_string();
}

static std::vector<json> HandleClientMessageImpl(const std::string& message);

std::vector<json> Device::HandleClientMessage(const std::string& message)
{
	// Directory iteration throws on I/O errors (permissions, a dir removed
	// mid-walk), and json throws on a client field of an unexpected type;
	// neither may take down the device thread.
	try
	{
		return HandleClientMessageImpl(message);
	}
	catch (const std::exception& e)
	{
		// Filesystem errors, or a json type error from an unexpected field type.
		std::printf("HandleClientMessage :: failed (%s)\n", e.what());
		return {};
	}
}

static std::vector<json> HandleClientMessageImpl(const std::string& message)
{
	using D = Device;
	// Runs on the device-I/O thread, so any device mutation triggered here is
	// single-threaded and safe. Parsing is fully defensive: a malformed or
	// unrecognized message is logged and ignored, never fatal. Returns messages
	// to broadcast in reply (e.g. a player's refreshed profile list).
	std::vector<json> replies;
	json j;
	try
	{
		j = json::parse(message);
	}
	catch (const std::exception& e)
	{
		std::printf("HandleClientMessage :: invalid JSON ignored (%s)\n", e.what());
		return replies;
	}

	if (!j.is_object())
	{
		std::printf("HandleClientMessage :: non-object message ignored\n");
		return replies;
	}

	std::string player;

	if (j.contains("rescanPlayers"))
	{
		Players::RequestRescan();
		return replies;
	}

	if (j.contains("listProfiles"))
	{
		if (ValidPlayer(j["listProfiles"], player))
			replies.push_back(PlayerProfilesJson(player));
		return replies;
	}

	// Everything else acts on one pad, named by its HID path in `pad`.
	PadDevice* device = nullptr;
	if (connectionManager && HasString(j, "pad"))
		device = connectionManager->Find(j["pad"].get<std::string>());
	if (!device)
	{
		std::printf("HandleClientMessage :: missing or unknown pad, message ignored\n");
		return replies;
	}
	connectionManager->SetCurrent(device);
	struct ClearCurrent
	{
		~ClearCurrent() { connectionManager->SetCurrent(nullptr); }
	} clearCurrent;

	const std::string padName = device->State().name;
	const int numSensors = device->State().numSensors;
	const auto padDir = [&] { return Profiles::PlayerPadDir(gProfilesDir, player, padName); };

	// ---- Profile ops: {pad, <op>:{player, ...}} on <player>/<this pad>/. ----
	// Profile files are on local disk, so this I/O on the device thread is fast.
	const json* op = nullptr;
	auto isOp = [&](const char* key) {
		if (!j.contains(key))
			return false;
		op = ValidPlayer(j[key], player) ? &j[key] : nullptr;
		return true;
	};
	auto refreshed = [&] {
		replies.push_back(PlayerProfilesJson(player));
		Players::RequestRescan(); // profile counts in the player list
	};

	if (isOp("loadProfile"))
	{
		json blob;
		if (!op || !HasString(*op, "id") || !Profiles::Read(padDir(), (*op)["id"].get<std::string>(), blob))
		{
			std::printf("loadProfile :: unknown profile, ignored\n");
			return replies;
		}
		if (blob.value("sensorCount", -1) != numSensors)
		{
			std::printf("loadProfile :: sensor-count mismatch, ignored\n");
			return replies;
		}
		try
		{
			D::LoadProfile(blob, PROFILE_GROUPS);
			SetPadSelection(padName, player, blob.value("id", std::string()), blob.value("name", std::string()));
		}
		catch (const std::exception& e)
		{
			std::printf("loadProfile :: failed (%s)\n", e.what());
		}
		return replies;
	}

	// Capture this pad's live settings. Two forms:
	//   {saveProfile:{player, name, description?}} -> new profile (refused at MAX_PER_PAD)
	//   {saveProfile:{player, id}}                 -> overwrite, keeping name/description
	if (isOp("saveProfile"))
	{
		if (!op)
			return replies;
		const auto dir = padDir();
		std::string id, name, description;
		if (HasString(*op, "id"))
		{
			json existing;
			if (!Profiles::Read(dir, (*op)["id"].get<std::string>(), existing))
			{
				std::printf("saveProfile :: unknown id, ignored\n");
				return replies;
			}
			id = existing.value("id", std::string());
			name = existing.value("name", std::string());
			description = existing.value("description", std::string());
		}
		else if (HasString(*op, "name"))
		{
			if (Profiles::List(dir).size() >= Profiles::MAX_PER_PAD)
			{
				std::printf("saveProfile :: %s already has %zu profiles on %s, refused\n", player.c_str(),
				            Profiles::MAX_PER_PAD, padName.c_str());
				return replies;
			}
			name = (*op)["name"].get<std::string>();
			description = (*op).value("description", std::string());
			id = NewProfileId(dir, name);
		}
		else
		{
			std::printf("saveProfile :: needs a name or id, ignored\n");
			return replies;
		}

		json blob;
		D::SaveProfile(blob, PROFILE_GROUPS); // capture this pad
		blob["id"] = id;
		blob["name"] = name;
		blob["description"] = description;
		blob["sensorCount"] = numSensors;
		blob["savedAt"] = (int64_t)std::time(nullptr);
		if (Profiles::Write(dir, id, blob))
		{
			SetPadSelection(padName, player, id, name); // that is what the pad holds now
			refreshed();
		}
		return replies;
	}

	// Store a profile the client read from any pad (this one: a duplicate; or
	// one on another server, since every server keeps only its own pads'
	// profiles) onto this pad under a new name:
	//   {pad, importProfile:{player, name, profile:{...stored profile...}}}
	if (isOp("importProfile"))
	{
		if (!op || !HasString(*op, "name") || !(*op).contains("profile"))
			return replies;
		json blob = Profiles::Sanitize((*op)["profile"]);
		const auto dir = padDir();
		if (blob.is_null() || blob["sensorCount"] != numSensors || Profiles::List(dir).size() >= Profiles::MAX_PER_PAD)
		{
			std::printf("importProfile :: invalid profile, sensor-count mismatch or pad full, refused\n");
			return replies;
		}
		const std::string name = (*op)["name"].get<std::string>();
		const std::string id = NewProfileId(dir, name);
		blob["id"] = id;
		blob["name"] = name;
		blob["savedAt"] = (int64_t)std::time(nullptr);
		if (Profiles::Write(dir, id, blob))
			refreshed();
		return replies;
	}

	// Edit metadata (name/description) only — never re-captures the pad.
	if (isOp("updateProfile"))
	{
		json blob;
		const auto dir = op ? padDir() : Profiles::Path();
		if (!op || !HasString(*op, "id") || !Profiles::Read(dir, (*op)["id"].get<std::string>(), blob))
			return replies;
		if (HasString(*op, "name"))
			blob["name"] = (*op)["name"].get<std::string>();
		if (HasString(*op, "description"))
			blob["description"] = (*op)["description"].get<std::string>();
		const std::string id = blob.value("id", std::string());
		if (Profiles::Write(dir, id, blob))
		{
			json& st = PadStateFor(padName);
			if (st.value("player", "") == player && st.value("profileId", "") == id)
			{
				st["profileName"] = blob.value("name", std::string());
				SavePadState(padName);
			}
			refreshed();
		}
		return replies;
	}

	if (isOp("deleteProfile"))
	{
		if (!op || !HasString(*op, "id"))
			return replies;
		const std::string id = (*op)["id"].get<std::string>();
		if (Profiles::Delete(padDir(), id))
		{
			json& st = PadStateFor(padName);
			if (st.value("player", "") == player && st.value("profileId", "") == id)
			{
				st["profileId"] = "";
				st["profileName"] = "";
				SavePadState(padName);
			}
			refreshed();
		}
		return replies;
	}

	// ---- Device ops. ----

	// Release mode: 0 = none, 1 = global, 2 = individual (ReleaseMode enum).
	if (j.contains("releaseMode") && j["releaseMode"].is_number_integer())
	{
		int mode = j["releaseMode"].get<int>();
		if (mode >= RELEASE_NONE && mode <= RELEASE_INDIVIDUAL)
			D::SetReleaseMode((ReleaseMode)mode);
		else
			std::printf("releaseMode :: out of range, ignored\n");
		return replies;
	}

	// Recalibrate one sensor's baseline; out-of-range indices are rejected downstream.
	if (j.contains("calibrateSensor") && j["calibrateSensor"].is_number_integer())
	{
		D::CalibrateSensor(j["calibrateSensor"].get<int>());
		return replies;
	}

	// A configuration update reuses the hardened LoadProfile path, which
	// validates every index and group before touching device state.
	if (j.contains("sensors") || j.contains("releaseThreshold") || j.contains("name") || j.contains("lightRules"))
	{
		try
		{
			D::LoadProfile(j, DGP_ALL);
		}
		catch (const std::exception& e)
		{
			std::printf("HandleClientMessage :: failed to apply config (%s)\n", e.what());
		}
		// Profiles are stored per pad name, so a rename carries them along.
		RenamePad(padName, device->State().name);
		return replies;
	}

	std::printf("HandleClientMessage :: unhandled message: %s\n", message.c_str());
	return replies;
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
	if (connectionManager->DiscoverDevice())
	{
		gLightsDirty = true;
		gPadStatesDirty = true;
	}
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
	j["devices"] = json::array();
	for (PadDevice* device : connectionManager ? connectionManager->Pads() : vector<PadDevice*>{})
	{
		json d;
		d["id"] = device->Path();
		d["name"] = device->State().name;
		d["sensorCount"] = device->State().numSensors;
		j["devices"].push_back(std::move(d));
	}
}

void Device::SetProfilesDir(const std::filesystem::path& dir)
{
	gProfilesDir = dir.empty() ? "profiles" : dir;
	std::printf("Profiles directory: %s\n", Profiles::PathU8(gProfilesDir).c_str());
	std::error_code ec;
	std::filesystem::create_directories(gProfilesDir, ec);
	if (ec)
		std::printf("SetProfilesDir :: could not create it (%s)\n", ec.message().c_str());
	gPadStates.clear();
	gPadStatesDirty = true;
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

			const bool hasTh = sensor.contains("threshold") && sensor["threshold"].is_number();
			const bool hasRel = sensor.contains("releaseThreshold") && sensor["releaseThreshold"].is_number();
			if ((groups & DPG_SENSITIVITY) && (hasTh || hasRel))
			{
				// Apply whichever fields the message carries; default the missing one
				// to the currently stored value. This lets a release-only patch (the
				// client's per-sensor release slider) actually reach the device instead
				// of being dropped for lacking a threshold. SetThreshold enforces the
				// active release mode on the stored release value.
				double th = hasTh ? sensor["threshold"].get<double>() : mySensors[idx].threshold;
				double rel = hasRel ? sensor["releaseThreshold"].get<double>() : mySensors[idx].releaseThreshold;
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
