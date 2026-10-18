#include <Adp.h>

#include <atomic>
#include <mutex>
#include <memory>
#include <algorithm>
#include <map>
#include <chrono>
#include <thread>
#include <ctime>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <future>
#include <optional>
#include <set>
#include <utility>

#include "hidapi.h"
#include <fmt/format.h>

#include <Model/Device.h>
#include <Model/PadDevice.h>
#include <Model/Reporter.h>
#include <Model/Wire.h>
#include <Model/Players.h>
#include <Model/Profiles.h>

using namespace std;
using namespace chrono;

namespace adp
{

struct HidIdentifier
{
	int vendorId;
	int productId;
};

constexpr HidIdentifier HID_IDS[] = {
    {0x1209, 0xb196}, // pid.codes id of the ADP firmware
    {0x03eb, 0x204f}, // Atmel's LUFA generic-HID id, used by older ADP firmware (other LUFA demos share it)
};

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

SensorReport SensorState::ToReport(int index, int max) const
{
	SensorReport report{}; // nothing left uninitialized goes to the device

	report.index = index;
	report.flags = WriteU16LE(flags);
	report.threshold = WriteU16LE(ToDeviceSensorValue(threshold, max));
	report.releaseThreshold = WriteU16LE(ToDeviceSensorValue(releaseThreshold, max));
	report.resistorValue = resistorValue;
	report.buttonMapping = button == 0 ? 0xFF : (button - 1);

	return report;
}

enum ConnectionState
{
	CS_UNKNOWN,
	CS_PROBED,
	CS_CONNECTED,
	CS_FAILED
};

// Stage 2 of connecting: reads every slot (~90 transfers) and builds the pad,
// or null if a read failed. Touches only `reporter`'s device.
static unique_ptr<PadDevice> BuildPad(const shared_ptr<Reporter>& reporter, const string& devicePath,
                                      const NameReport& name, const IdentificationV2Report& identification)
{
	// Firmware before v1.3 has no IdentificationV2 report, so Probe() already
	// refused it: everything below assumes the v1.3+ reports.

	// Reads every slot of one kind: select slot i, then get it.
	auto readSlots = [&](SetPropertyReport::Ids select, int count, auto& report, auto&& keep) {
		SetPropertyReport selectReport;
		selectReport.propertyId = WriteU32LE(select);
		for (int i = 0; i < count; ++i)
		{
			selectReport.propertyValue = WriteU32LE(i);
			if (!reporter->Send(selectReport) || !reporter->Get(report))
				return false;
			keep(report);
		}
		return true;
	};

	vector<LightRuleReport> lightRules;
	vector<LedMappingReport> ledMappings;
	if (identification.ledCount > 0)
	{
		LightRuleReport lightReport;
		LedMappingReport ledReport;
		if (!readSlots(SetPropertyReport::SELECTED_LIGHT_RULE_INDEX, MAX_LIGHT_RULES, lightReport,
		               [&](const LightRuleReport& r) {
			               if (r.flags & LRF_ENABLED)
				               lightRules.push_back(r);
		               }) ||
		    !readSlots(SetPropertyReport::SELECTED_LED_MAPPING_INDEX, MAX_LED_MAPPINGS, ledReport,
		               [&](const LedMappingReport& r) {
			               if (r.flags & LMF_ENABLED)
				               ledMappings.push_back(r);
		               }))
			return nullptr;
	}

	vector<SensorReport> sensors;
	SensorReport sensorReport;
	if (!readSlots(SetPropertyReport::SELECTED_SENSOR_INDEX, identification.sensorCount, sensorReport,
	               [&](const SensorReport& r) { sensors.push_back(r); }))
		return nullptr;

	auto device =
	    make_unique<PadDevice>(reporter, devicePath.c_str(), name, identification, lightRules, ledMappings, sensors);

	const PadState& pad = device->State();
	std::printf("ConnectionManager :: connected '%s' (%s, firmware v%u.%u, %d sensors, %d buttons, features %s) "
	            "at %s\n",
	            pad.name.c_str(), pad.boardType.c_str(), pad.firmwareVersion.major, pad.firmwareVersion.minor,
	            pad.numSensors, pad.numButtons, fmt::format("{:b}", ReadU16LE(identification.features)).c_str(),
	            devicePath.c_str());
	return device;
}

class DeviceConnection
{
  public:
	DeviceConnection(std::string path) : path(path) {}

	// The hid_device* is owned solely by the Reporter's BackendHid (which closes
	// it in its destructor), so there is no handle to close here. A running
	// stage 2 is waited for (by the future's destructor).
	~DeviceConnection() = default;

	// Opens the pad and reads its name and identification (kept for stage 2).
	// Stays on the device thread, where hid_enumerate runs too.
	bool Probe()
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		stage2Failures = 0;

		hid_device* handle = hid_open_path(path.c_str());
		if (!handle)
		{
			std::printf("DeviceConnection :: hid_open failed (%ls) :: %s\n", hid_error(nullptr), path.c_str());
			SetFailed();
			return false;
		}

		if (hid_set_nonblocking(handle, 1) < 0)
		{
			std::printf("DeviceConnection :: hid_set_nonblocking failed :: %s\n", path.c_str());
			hid_close(handle);
			SetFailed();
			return false;
		}

		// Reporter/BackendHid takes ownership of the handle from here on.
		reporter = make_shared<Reporter>(handle);

		// IdentificationV2 exists from firmware v1.3 on; older firmware is not
		// supported (it fails here, and is retried with a backoff).
		if (!reporter->Get(name) || !reporter->Get(identification))
		{
			SetFailed();
			return false;
		}
		ClampIdentification(identification);

		state = CS_PROBED;
		return true;
	}

	ConnectionState GetState() { return state; }

	// Runs stage 2 on a worker thread, so connecting a pad doesn't stall polling
	// of the others. Until TakeStage2 returns its result the worker owns the HID
	// handle, and this connection is left alone (the worker reads its members).
	void StartStage2()
	{
		stage2 = std::async(std::launch::async, [this] { return BuildPad(reporter, path, name, identification); });
	}

	bool Stage2Pending() const { return stage2.valid(); }

	// True once a started stage 2 is done, with `pad` its result (null if it failed).
	bool TakeStage2(unique_ptr<PadDevice>& pad)
	{
		if (!stage2.valid() || stage2.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return false;
		try
		{
			pad = stage2.get();
		}
		catch (const std::exception& e)
		{
			std::printf("DeviceConnection :: stage 2 failed (%s)\n", e.what());
			pad = nullptr;
		}
		return true;
	}

	void SetConnected()
	{
		state = CS_CONNECTED;
		failures = 0;
	}

	// Stage 2 (reading every slot) failed. It is retried on the next discovery
	// pass (~1s); after 3 tries the pad goes into the failed backoff.
	void Stage2Failed()
	{
		std::printf("DeviceConnection :: ConnectStage2 failed (%d)\n", stage2Failures);
		if (++stage2Failures >= 3)
			SetFailed();
	}

	// Closes the handle (unless a PadDevice still shares it) and remembers when,
	// so discovery can retry after a backoff.
	void SetFailed()
	{
		state = CS_FAILED;
		failedAt = std::chrono::steady_clock::now();
		++failures;
		reporter.reset();
		std::printf("DeviceConnection :: %s failed, retrying in %llds\n", path.c_str(),
		            (long long)RetryDelay().count());
	}

	// Failed long enough ago to be probed again (e.g. hidraw permissions were
	// not applied yet when the pad was plugged in, or a transient HID error).
	bool RetryDue() const { return state == CS_FAILED && std::chrono::steady_clock::now() - failedAt > RetryDelay(); }

	// 5s after the first failure, doubling up to 5 minutes: a pad recovers
	// quickly, while a matching non-ADP device (0x03eb:0x204f is LUFA's
	// generic id) stops being probed and logged every few seconds.
	std::chrono::seconds RetryDelay() const
	{
		return std::min(std::chrono::seconds(5) * (1 << std::min(failures - 1, 6)), std::chrono::seconds(300));
	}

  protected:
	ConnectionState state = CS_UNKNOWN;
	std::chrono::steady_clock::time_point failedAt;
	int failures = 0;       // consecutive, reset on a successful connect
	int stage2Failures = 0; // since the last probe
	std::string path;
	NameReport name;
	IdentificationV2Report identification;

	// Shared with the PadDevice created from this connection.
	shared_ptr<Reporter> reporter;
	std::future<unique_ptr<PadDevice>> stage2;
};

class ConnectionManager
{
  public:
	// Retiring pads finish their save in their destructors.
	~ConnectionManager()
	{
		for (auto& [path, pad] : myPads)
			pad->SaveChanges();
	}

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

	static std::vector<string> Enumerate()
	{
		std::vector<string> devicePaths;
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
		return devicePaths;
	}

	// Enumerates on a worker (it can take tens of ms on Windows, a hitch in
	// every pad's live values) every ~1s, and once it is done drops unplugged
	// pads and starts connecting new ones (Collect finishes that). Opening a
	// pad stays on this thread and never overlaps an enumeration. True if a
	// pad went away.
	bool DiscoverDevice()
	{
		constexpr auto interval = std::chrono::seconds(1);
		if (!enumeration.valid())
		{
			if (std::chrono::steady_clock::now() - lastEnumeration >= interval)
			{
				lastEnumeration = std::chrono::steady_clock::now();
				enumeration = std::async(std::launch::async, Enumerate);
			}
			return false;
		}
		if (enumeration.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return false;
		const std::vector<string> devicePaths = enumeration.get();

		// Remove any devices that are no longer connected. One whose stage 2 is
		// still running goes once that is done (its pad then fails its first read).
		bool changed = false;
		for (auto it = devices.begin(); it != devices.end();)
		{
			if (!it->second.Stage2Pending() &&
			    std::find(devicePaths.begin(), devicePaths.end(), it->first) == devicePaths.end())
			{
				changed |= myPads.contains(it->first);
				DropPad(it->first);
				// Unplugged: its RAM, and the edits, are gone.
				if (auto unsaved = unsavedPaths.find(it->first); unsaved != unsavedPaths.end())
				{
					lostEdits.push_back(unsaved->second);
					unsavedPaths.erase(unsaved);
				}

				std::printf("ConnectionManager :: device removed (%s)\n", it->first.c_str());
				it = devices.erase(it);
			}
			else
				++it;
		}

		// Probe and connect every newly seen pad, and failed ones whose backoff is
		// over, and finish probed ones whose stage 2 failed last time; all pads
		// are polled every tick.
		for (auto& path : devicePaths)
		{
			auto [it, isNew] = devices.try_emplace(path, path);
			DeviceConnection& con = it->second;
			if (con.Stage2Pending())
				continue;
			if (isNew || con.RetryDue())
			{
				if (!con.Probe())
					continue;
			}
			else if (con.GetState() != CS_PROBED)
				continue;

			con.StartStage2();
		}

		return changed;
	}

	// Takes finished background work: connected pads, finished saves, and
	// dropped pads whose last save is done. True if a pad came.
	bool Collect()
	{
		bool changed = false;
		for (auto& [path, con] : devices)
		{
			unique_ptr<PadDevice> pad;
			if (!con.TakeStage2(pad))
				continue;
			if (!pad)
			{
				con.Stage2Failed();
				continue;
			}
			// Back after a failed read that left edits in the pad's RAM unsaved.
			if (unsavedPaths.erase(path))
				pad->NotifyUnsavedChanges();
			// ponytail: a pad back on another path (another USB port) after a
			// failed save counts as lost; key by serial number if that shows.
			myPads[path] = std::move(pad);
			con.SetConnected();
			changed = true;
		}

		for (auto& [path, pad] : myPads)
			pad->FinishSave();

		// ponytail: a pad back on the same path before its retiring save is done
		// isn't told it has unsaved edits; the retry backoff (>=5s) outlasts a save.
		std::erase_if(myRetiring, [&](Retiring& r) {
			r.pad->FinishSave();
			if (r.pad->Saving())
				return false;
			if (r.pad->HasUnsavedChanges())
			{
				if (devices.contains(r.path))
					unsavedPaths[r.path] = r.pad->State().name;
				else
					lostEdits.push_back(r.pad->State().name); // unplugged before its save
			}
			return true;
		});
		return changed;
	}

	// Names of pads whose edits never reached their EEPROM (unplugged before
	// the save) since the last call.
	std::vector<string> TakeLostEdits() { return std::exchange(lostEdits, {}); }

	bool BackgroundWork() const
	{
		return !myRetiring.empty() ||
		       std::any_of(myPads.begin(), myPads.end(), [](const auto& p) { return p.second->Saving(); }) ||
		       std::any_of(devices.begin(), devices.end(), [](const auto& d) { return d.second.Stage2Pending(); });
	}

	void AddPad(unique_ptr<PadDevice> pad)
	{
		const string path = pad->Path();
		myPads[path] = std::move(pad);
	}

	// A HID read failed. The pad may only have hiccupped and keep its RAM.
	void DisconnectFailedDevice(const string& path)
	{
		if (devices.contains(path))
			devices.at(path).SetFailed();
		DropPad(path);
	}

	// Takes the pad out of service. Its unsaved edits are saved in the
	// background (a dead pad can take the whole HID timeout to refuse); if that
	// fails too they are remembered and saved once it reconnects, unless it
	// was unplugged meanwhile.
	void DropPad(const string& path)
	{
		auto it = myPads.find(path);
		if (it == myPads.end())
			return;
		it->second->StartSave();
		myRetiring.push_back({path, std::move(it->second)});
		myPads.erase(it);
	}

  private:
	struct Retiring
	{
		DevicePath path;
		unique_ptr<PadDevice> pad;
	};

	map<DevicePath, DeviceConnection> devices;
	map<DevicePath, string> unsavedPaths; // -> pad name: edits in the pad's RAM only
	vector<string> lostEdits;             // see TakeLostEdits

	map<DevicePath, unique_ptr<PadDevice>> myPads;
	vector<Retiring> myRetiring; // dropped, their last save still running

	std::future<std::vector<string>> enumeration; // a running Enumerate
	std::chrono::steady_clock::time_point lastEnumeration;
};

// ====================================================================================================================
// Device API.
// ====================================================================================================================

static std::unique_ptr<ConnectionManager> connectionManager;

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

static void FollowPlayerRenames(const std::vector<Players::Rename>& renames);
static void SetEditsLost(const std::string& pad, bool lost);

void Device::Update()
{
	DeviceChanges changes = 0;

	// Pads connected in the background, finished saves.
	if (connectionManager->Collect())
		changes |= DCF_DEVICE;
	for (const string& pad : connectionManager->TakeLostEdits())
	{
		std::printf("ConnectionManager :: '%s' was unplugged before its edits were saved\n", pad.c_str());
		SetEditsLost(pad, true);
	}

	// Poll every pad. HID access happens here on the device-I/O thread, except
	// for a pad's background save or connect (see PadDevice::StartSave).
	for (PadDevice* device : connectionManager->Pads())
	{
		// Its HID handle belongs to the save running in the background.
		if (device->Saving())
			continue;
		const string path = device->Path(); // the device is gone after a disconnect
		DeviceChanges padChanges = device->PopChanges();

		if (!device->PollSensors())
		{
			connectionManager->DisconnectFailedDevice(path);
			changes |= DCF_DEVICE;
			continue;
		}

		changes |= padChanges;
	}

	// In the background: the firmware writes its EEPROM inside the request,
	// which can take a second or more, and must stall only that pad.
	for (PadDevice* device : connectionManager->Pads())
		if (device->SaveDue())
			device->StartSave();

	FollowPlayerRenames(Players::TakeRenames());

	// A lights change (or pads coming/going) means the gated messages are stale.
	if (changes & (DCF_LIGHTS | DCF_DEVICE))
		gLightsDirty = true;
	if (changes & (DCF_DEVICE | DCF_NAME))
		gPadStatesDirty = true;

	// Publish the latest immutable snapshots for the WebSocket side to read.
	PublishSnapshot();
}

void Device::PublishSnapshot()
{
	auto snapshots = std::make_shared<std::vector<SensorSnapshot>>();

	for (PadDevice* device : connectionManager ? connectionManager->Pads() : vector<PadDevice*>{})
	{
		SensorSnapshot snapshot;
		const PadState& pad = device->State();
		snapshot.path = device->Path();
		snapshot.pollingRate = device->PollingRate();
		snapshot.releaseMode = (int)pad.releaseMode;
		snapshot.maxSensorValue = pad.maxSensorValue;
		snapshot.unsaved = device->HasUnsavedChanges();
		snapshot.saveFailed = device->SaveFailed();
		snapshot.saving = device->Saving();

		snapshot.sensors.reserve(pad.numSensors);
		for (int i = 0; i < pad.numSensors; ++i)
		{
			if (const SensorState* s = device->Sensor(i))
				snapshot.sensors.push_back(*s);
		}

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
	j["pollingRate"] = snapshot.pollingRate;
	j["releaseMode"] = snapshot.releaseMode;
	j["unsaved"] = snapshot.unsaved;
	j["saveFailed"] = snapshot.saveFailed;
	j["saving"] = snapshot.saving;
	j["sensors"] = json::array();

	// Values are averages over the tick, so quantize them to the device's own
	// resolution (1/maxSensorValue): a pad at rest then serializes identically
	// tick to tick (letting the loop skip unchanged snapshots), and 4 decimals
	// replace 17 digits. 4 decimals still round-trip every device value exactly
	// (up to MAX_REPORTED_SENSOR_VALUE: 0.00005 * 4095 < 0.5).
	const auto q = [](double v) { return std::round(v * 1e4) / 1e4; };
	const double max = snapshot.maxSensorValue;
	const auto device = [&](double v) { return std::round(v * max) / max; };
	for (const SensorState& s : snapshot.sensors)
	{
		json sensor;
		sensor["threshold"] = q(s.threshold);
		sensor["releaseThreshold"] = q(s.releaseThreshold);
		sensor["value"] = q(device(s.value));
		sensor["resistorValue"] = s.resistorValue;
		sensor["button"] = s.button;
		sensor["pressed"] = s.pressed;
		sensor["disabled"] = (s.flags & SensorReport::ADC_DISABLED) != 0; // the firmware doesn't read it
		j["sensors"].push_back(std::move(sensor));
	}
}

// Lights change rarely and are ~1KB, so they travel as their own message
// (msgType 4) that the WebSocket loop broadcasts only on change — not with
// every 60Hz snapshot. Same shape a saved profile uses, so the client can
// drift-check a loaded profile's lights field-for-field.
void Device::LightsToJson(const std::string& pad, const LightsState& lights, json& j)
{
	j["msgType"] = 4;
	j["pad"] = pad;
	AppendLightsToJson(lights, j);
}

std::vector<std::string> Device::LightsMessages()
{
	std::vector<std::string> out;
	for (PadDevice* device : connectionManager ? connectionManager->Pads() : vector<PadDevice*>{})
	{
		json j;
		LightsToJson(device->Path(), device->State().featureLights ? device->Lights() : LightsState{}, j);
		out.push_back(Profiles::Dump(j));
	}
	return out;
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

// Flags (or clears) that the pad's last edits never reached its EEPROM: it
// was unplugged before they were saved, so it came back with older settings.
// Shown until settings are applied to it again.
static void SetEditsLost(const std::string& pad, bool lost)
{
	json& st = PadStateFor(pad);
	if (st.value("editsLost", false) == lost)
		return;
	if (lost)
		st["editsLost"] = true;
	else
		st.erase("editsLost");
	SavePadState(pad);
}

// Marks `player`/`id` as the pad's current selection and moves the player to
// the front of the pad's recent list.
static void SetPadSelection(const std::string& pad, const std::string& player, const std::string& id,
                            const std::string& profileName)
{
	json& st = PadStateFor(pad);
	st.erase("editsLost");
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

static void RenamePadDirs(const std::string& from, const std::string& to);

// A pad was renamed: move its profile dirs (every player) and its state along.
// ponytail: walks all player dirs on the device thread; renaming is a rare operator action.
static void RenamePad(const std::string& from, const std::string& to)
{
	if (from == to || from.empty() || to.empty())
		return;
	// The pad already holds the new name, so a failure here is logged, never
	// reported as a failed rename.
	try
	{
		RenamePadDirs(from, to);
	}
	catch (const std::exception& e)
	{
		std::printf("RenamePad :: moving the profiles of '%s' to '%s' failed (%s)\n", from.c_str(), to.c_str(),
		            e.what());
	}
	gPadStates.erase(from);
	gPadStates.erase(to);
	gPadStatesDirty = true;
	Players::RequestRecount(); // every player's counts are keyed by pad name
	std::printf("Pad renamed '%s' -> '%s'\n", from.c_str(), to.c_str());
}

static void RenamePadDirs(const std::string& from, const std::string& to)
{
	std::error_code ec;
	const auto escFrom = Profiles::U8Path(Profiles::EscapeName(from));
	const auto escTo = Profiles::U8Path(Profiles::EscapeName(to));
	// Moves one dir, logging (not hiding) why it stayed put. On a
	// case-insensitive filesystem a case-only rename finds dst "existing" as src
	// itself; that one is moved.
	auto move = [](const Profiles::Path& src, const Profiles::Path& dst) {
		std::error_code ec;
		if (!std::filesystem::exists(src, ec))
			return;
		if (std::filesystem::exists(dst, ec) && !std::filesystem::equivalent(src, dst, ec))
			std::printf("RenamePad :: %s already exists, %s NOT moved\n", Profiles::PathU8(dst).c_str(),
			            Profiles::PathU8(src).c_str());
		else if (std::filesystem::rename(src, dst, ec); ec)
			std::printf("RenamePad :: moving %s failed (%s)\n", Profiles::PathU8(src).c_str(), ec.message().c_str());
	};
	for (const auto& player : std::filesystem::directory_iterator(gProfilesDir / "players", ec))
		move(player.path() / escFrom, player.path() / escTo);
	move(PadStateDir(from), PadStateDir(to));
}

// Why renaming pad `from` to `to` would lose data, or "" if it is safe: both
// names hold a profile dir for the same player (or both a pad state), so
// RenamePad couldn't move them. Taking over the name of a pad whose data is
// on disk is fine when this pad has none of its own (a replaced board
// adopting the old pad's profiles).
static std::string RenameConflict(const std::string& from, const std::string& to)
{
	std::error_code ec;
	const auto escFrom = Profiles::U8Path(Profiles::EscapeName(from));
	const auto escTo = Profiles::U8Path(Profiles::EscapeName(to));
	auto both = [&](const Profiles::Path& a, const Profiles::Path& b) {
		return std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec) &&
		       !std::filesystem::equivalent(a, b, ec); // a case-only rename on a case-insensitive fs
	};
	for (const auto& player : std::filesystem::directory_iterator(gProfilesDir / "players", ec))
		if (both(player.path() / escFrom, player.path() / escTo))
			return fmt::format("{} has profiles on both '{}' and '{}'",
			                   Profiles::UnescapeName(Profiles::PathU8(player.path().filename())), from, to);
	if (both(PadStateDir(from), PadStateDir(to)))
		return fmt::format("a pad named '{}' has been used here before", to);
	return "";
}

// A LocalProfiles player was renamed (the scanner already moved their profile
// dir): follow it in every pad's selection and recent list. Walks the pad
// states on disk, so pads that aren't plugged in right now follow it too.
static void FollowPlayerRenames(const std::vector<Players::Rename>& renames)
{
	if (renames.empty())
		return;
	try
	{
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(gProfilesDir / "pads", ec))
		{
			const std::string pad = Profiles::UnescapeName(Profiles::PathU8(entry.path().filename()));
			json& st = PadStateFor(pad);
			bool changed = false;
			for (const auto& r : renames)
			{
				if (st.contains("player") && st["player"] == r.from)
				{
					st["player"] = r.to;
					changed = true;
				}
				if (st.contains("recent") && st["recent"].is_array())
					for (auto& p : st["recent"])
						if (p == r.from)
						{
							p = r.to;
							changed = true;
						}
			}
			if (changed)
				SavePadState(pad);
		}
	}
	catch (const std::exception& e)
	{
		std::printf("FollowPlayerRenames :: failed (%s)\n", e.what());
	}
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

// Why a profile op's name or description can't be stored, or "".
static std::string BadText(const json& op)
{
	if (HasString(op, "name") && op["name"].get_ref<const std::string&>().size() > Profiles::MAX_NAME_BYTES)
		return fmt::format("the name is longer than {} bytes", Profiles::MAX_NAME_BYTES);
	if (HasString(op, "name") && Profiles::Blank(op["name"].get_ref<const std::string&>()))
		return "the name must not be empty";
	if (HasString(op, "description") &&
	    op["description"].get_ref<const std::string&>().size() > Profiles::MAX_DESCRIPTION_BYTES)
		return fmt::format("the description is longer than {} bytes", Profiles::MAX_DESCRIPTION_BYTES);
	return "";
}

// Longest button name, in code points: it has to fit a narrow box on a phone.
static constexpr int MAX_BUTTON_NAME = 8;

static constexpr const char* PROFILE_OPS[] = {"loadProfile", "saveProfile", "importProfile", "updateProfile",
                                              "deleteProfile"};

// How many different ops a message asks for. More than one is refused rather
// than one picked by priority: a configure that also carries a name must not
// turn into a rename and silently drop its sensors.
static int OpCount(const json& j)
{
	int n = 0;
	for (const char* key : PROFILE_OPS)
		n += j.contains(key);
	n += j.contains("releaseMode");
	n += j.contains("name");
	n += j.contains("buttonName");
	n += j.contains("sensors") || j.contains("lightRules") || j.contains("ledMappings");
	return n;
}

// The op a pad message asks for, as named in its result (msgType 8), or "".
// Meaningful only if OpCount(j) == 1.
static std::string OpNameOf(const json& j)
{
	for (const char* key : PROFILE_OPS)
		if (j.contains(key))
			return key;
	if (j.contains("releaseMode"))
		return "setReleaseMode";
	if (j.contains("name"))
		return "rename";
	if (j.contains("buttonName"))
		return "renameButton";
	if (j.contains("sensors") || j.contains("lightRules") || j.contains("ledMappings"))
		return "configure";
	return "";
}

// Another connected pad named `name` (ignoring case), or nullptr. Profiles
// and the pad's selection are stored under its name, so two pads sharing one
// (e.g. both still on the factory default) would share them too, and on a
// case-insensitive filesystem (Windows, macOS) so would "PAD" and "pad".
static PadDevice* OtherPadNamed(const PadDevice* device, const std::string& name)
{
	const std::string folded = Profiles::FoldName(name);
	for (PadDevice* other : connectionManager->Pads())
		if (other != device && Profiles::FoldName(other->State().name) == folded)
			return other;
	return nullptr;
}

// The error of an op whose settings only partly reached the pad, naming what
// didn't (LoadProfile's `failed`).
static std::string NotApplied(const std::vector<std::string>& failed)
{
	std::string what;
	for (const auto& f : failed)
		what += (what.empty() ? "" : ", ") + f;
	return what.empty() ? "not every setting reached the pad" : "not every setting reached the pad (" + what + ")";
}

// The op being handled, so a failure can still be answered.
struct OpContext
{
	std::string op;
	json pad;
	json requestId; // echoed back if the client sent one
};

// The op being answered, read from the message itself.
static OpContext ContextOf(const json& j)
{
	OpContext ctx;
	ctx.op = OpNameOf(j);
	if (HasString(j, "pad"))
		ctx.pad = j["pad"];
	if (j.contains("requestId"))
		ctx.requestId = j["requestId"];
	return ctx;
}

// The answer to an op, for its sender (msgType 8).
static ClientReply OpResult(const OpContext& ctx, bool ok, const std::string& error, const std::string& name = "")
{
	json result = {{"msgType", 8}, {"op", ctx.op}, {"pad", ctx.pad}, {"ok", ok}, {"error", error}, {"name", name}};
	if (!ctx.requestId.is_null())
		result["requestId"] = ctx.requestId;
	return {std::move(result), true};
}

static std::vector<ClientReply> HandleClientMessageImpl(const json& j, OpContext& ctx);

std::vector<ClientReply> Device::HandleClientMessage(const std::string& message)
{
	return HandleParsedMessage(json::parse(message, nullptr, false));
}

bool Device::IsConfigure(const json& j)
{
	return j.is_object() && HasString(j, "pad") && OpCount(j) == 1 && OpNameOf(j) == "configure";
}

bool Device::MergeConfigure(json& into, const json& next)
{
	json merged = into;
	const bool intoSensors = into.contains("sensors"), nextSensors = next.contains("sensors");
	if (intoSensors && nextSensors)
	{
		json& a = merged["sensors"];
		const json& b = next["sensors"];
		// Different lengths: at least one is refused as a whole, so keep both.
		if (!a.is_array() || !b.is_array() || a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i)
		{
			if (a[i].is_object() && b[i].is_object())
				a[i].update(b[i]);
			else if (b[i].is_object())
				a[i] = b[i];
		}
	}
	for (const auto& [key, value] : next.items())
		if (key != "sensors" || !intoSensors)
			merged[key] = value; // lights arrays configure every slot, so the later wins

	// Later lights replace earlier ones: if the pad would refuse the later
	// (all or nothing), folding would lose the earlier valid ones.
	if (next.contains("lightRules") || next.contains("ledMappings"))
	{
		const PadDevice* pad = connectionManager && HasString(next, "pad")
		                           ? connectionManager->Find(next["pad"].get<std::string>())
		                           : nullptr;
		std::vector<std::optional<LedMapping>> mappings;
		std::vector<std::optional<LightRule>> rules;
		if (!pad || !pad->ParseLights(next, mappings, rules) || !pad->UnlitMapping(merged).empty())
			return false;
	}
	into = std::move(merged);
	return true;
}

std::vector<ClientReply> Device::Refuse(const json& j, const std::string& error)
{
	if (!j.is_object())
		return {};
	const OpContext ctx = ContextOf(j);
	if (ctx.op.empty())
		return {};
	return {OpResult(ctx, false, error)};
}

std::vector<ClientReply> Device::HandleParsedMessage(const json& message)
{
	// Directory iteration throws on I/O errors (permissions, a dir removed
	// mid-walk), and json throws on a client field of an unexpected type;
	// neither may take down the device thread.
	OpContext ctx;
	try
	{
		return HandleClientMessageImpl(message, ctx);
	}
	catch (const std::exception& e)
	{
		// Filesystem errors, or a json type error from an unexpected field type.
		// Writes made before the throw stay applied.
		std::printf("HandleClientMessage :: failed (%s)\n", e.what());
		if (ctx.op.empty())
			return {};
		return {OpResult(ctx, false, "invalid message")};
	}
}

static std::vector<ClientReply> HandleClientMessageImpl(const json& j, OpContext& ctx)
{
	// Runs on the device-I/O thread, so any device mutation triggered here is
	// single-threaded and safe. Parsing is fully defensive: a malformed or
	// unrecognized message is logged and ignored, never fatal. Returns the
	// replies: a profile list for the asker, or for everyone after a change,
	// and a result (msgType 8) for the asker after every op.
	std::vector<ClientReply> replies;
	if (j.is_discarded())
	{
		std::printf("HandleClientMessage :: invalid JSON ignored\n");
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

	// players/ is where the scanner renames player dirs, so whatever reads or
	// writes it holds the store lock. Sensor and lights edits (slider rate,
	// HID round-trips) don't.
	std::unique_lock<std::mutex> storeLock(Profiles::StoreMutex(), std::defer_lock);

	if (j.contains("listProfiles"))
	{
		storeLock.lock();
		if (ValidPlayer(j["listProfiles"], player))
			replies.push_back({PlayerProfilesJson(player), true});
		return replies;
	}

	// Everything else acts on one pad, named by its HID path in `pad`, and is
	// answered with {msgType 8, op, pad, ok, error, name}.
	ctx = ContextOf(j);
	const std::string& opName = ctx.op;
	auto done = [&](bool ok, const std::string& error, const std::string& name = "") {
		if (!ok)
			std::printf("%s :: %s\n", opName.c_str(), error.c_str());
		replies.push_back(OpResult(ctx, ok, error, name));
		return replies;
	};
	if (OpCount(j) > 1)
		return done(false, "the message asks for more than one op");

	PadDevice* device = nullptr;
	if (connectionManager && HasString(j, "pad"))
		device = connectionManager->Find(j["pad"].get<std::string>());
	if (!device)
	{
		if (opName.empty())
		{
			std::printf("HandleClientMessage :: missing or unknown pad, message ignored\n");
			return replies;
		}
		return done(false, "pad not connected");
	}
	// Normally held back while it saves (BusyPads): until then its HID handle
	// is the save's.
	device->FinishSave(true);
	const std::string padName = device->State().name;
	const int numSensors = device->State().numSensors;
	const auto padDir = [&] { return Profiles::PlayerPadDir(gProfilesDir, player, padName); };

	// ---- Profile ops: {pad, <op>:{player, ...}} on <player>/<this pad>/. ----
	// Profile files are on local disk, so this I/O on the device thread is fast.
	const bool profileOp = std::find(std::begin(PROFILE_OPS), std::end(PROFILE_OPS), opName) != std::end(PROFILE_OPS);
	if (profileOp || opName == "rename")
		storeLock.lock();

	const json* op = nullptr;
	if (profileOp)
	{
		if (!ValidPlayer(j[opName], player))
			return done(false, "unknown player");
		op = &j[opName];
		// This also catches a rename the pad stored differently than asked.
		if (OtherPadNamed(device, padName))
			return done(false, fmt::format("another pad here is also named '{}', rename one first", padName));
		if (const std::string error = BadText(*op); !error.empty())
			return done(false, error);
	}

	auto refreshed = [&] {
		// Broadcast: other clients may be showing this player's profiles too.
		replies.push_back({PlayerProfilesJson(player), false});
		Players::RequestRecount(player); // profile counts in the player list
	};
	const auto full = [&] {
		return fmt::format("{} already has {} profiles on this pad", player, Profiles::MAX_PER_PAD);
	};

	if (opName == "loadProfile")
	{
		json blob;
		if (!HasString(*op, "id") || !Profiles::ReadProfile(padDir(), (*op)["id"].get<std::string>(), blob))
			return done(false, "unknown profile");
		if (blob.value("sensorCount", -1) != numSensors)
			return done(false, "profile is for a pad with a different number of sensors");
		std::vector<std::string> failed;
		const bool applied = device->LoadProfile(blob, PROFILE_GROUPS, &failed);
		// Select it even if a write failed: most of it is on the pad, and the
		// client shows the drift.
		SetPadSelection(padName, player, blob.value("id", std::string()), blob.value("name", std::string()));
		return done(applied, applied ? "" : NotApplied(failed), blob.value("name", std::string()));
	}

	// Capture this pad's live settings. Two forms:
	//   {saveProfile:{player, name, description?}} -> new profile (refused at MAX_PER_PAD)
	//   {saveProfile:{player, id}}                 -> overwrite, keeping name/description
	if (opName == "saveProfile")
	{
		const auto dir = padDir();
		std::string id, name, description;
		if (HasString(*op, "id"))
		{
			json existing;
			if (!Profiles::ReadProfile(dir, (*op)["id"].get<std::string>(), existing))
				return done(false, "unknown profile");
			id = existing.value("id", std::string());
			name = existing.value("name", std::string());
			description = existing.value("description", std::string());
		}
		else if (HasString(*op, "name"))
		{
			if (Profiles::List(dir).size() >= Profiles::MAX_PER_PAD)
				return done(false, full());
			name = (*op)["name"].get<std::string>();
			description = (*op).value("description", std::string());
			id = NewProfileId(dir, name);
		}
		else
			return done(false, "needs a name or id");

		json blob;
		device->SaveProfile(blob, PROFILE_GROUPS); // capture this pad
		blob["id"] = id;
		blob["name"] = name;
		blob["description"] = description;
		blob["sensorCount"] = numSensors;
		blob["savedAt"] = (int64_t)std::time(nullptr);
		if (!Profiles::Write(dir, id, blob))
			return done(false, "could not write the profile");
		SetPadSelection(padName, player, id, name); // that is what the pad holds now
		refreshed();
		return done(true, "", name);
	}

	// Store a profile the client read from any pad (this one: a duplicate; or
	// one on another server, since every server keeps only its own pads'
	// profiles) onto this pad under a new name:
	//   {pad, importProfile:{player, name, profile:{...stored profile...}}}
	if (opName == "importProfile")
	{
		if (!HasString(*op, "name") || !(*op).contains("profile"))
			return done(false, "needs a name and a profile");
		json blob = Profiles::Sanitize((*op)["profile"]);
		const auto dir = padDir();
		if (blob.is_null())
			return done(false, "invalid profile");
		if (blob["sensorCount"] != numSensors)
			return done(false, "profile is for a pad with a different number of sensors");
		if (Profiles::List(dir).size() >= Profiles::MAX_PER_PAD)
			return done(false, full());
		const std::string name = (*op)["name"].get<std::string>();
		const std::string id = NewProfileId(dir, name);
		blob["id"] = id;
		blob["name"] = name;
		blob["savedAt"] = (int64_t)std::time(nullptr);
		if (!Profiles::Write(dir, id, blob))
			return done(false, "could not write the profile");
		refreshed();
		return done(true, "", name);
	}

	// Edit metadata (name/description) only — never re-captures the pad.
	if (opName == "updateProfile")
	{
		json blob;
		const auto dir = padDir();
		if (!HasString(*op, "id") || !Profiles::ReadProfile(dir, (*op)["id"].get<std::string>(), blob))
			return done(false, "unknown profile");
		if (HasString(*op, "name"))
			blob["name"] = (*op)["name"].get<std::string>();
		if (HasString(*op, "description"))
			blob["description"] = (*op)["description"].get<std::string>();
		const std::string id = blob.value("id", std::string());
		if (!Profiles::Write(dir, id, blob))
			return done(false, "could not write the profile");
		json& st = PadStateFor(padName);
		if (st.value("player", "") == player && st.value("profileId", "") == id)
		{
			st["profileName"] = blob.value("name", std::string());
			SavePadState(padName);
		}
		refreshed();
		return done(true, "", blob.value("name", std::string()));
	}

	if (opName == "deleteProfile")
	{
		if (!HasString(*op, "id"))
			return done(false, "needs an id");
		const std::string id = (*op)["id"].get<std::string>();
		if (!Profiles::Delete(padDir(), id))
			return done(false, "could not delete the profile");
		json& st = PadStateFor(padName);
		if (st.value("player", "") == player && st.value("profileId", "") == id)
		{
			st["profileId"] = "";
			st["profileName"] = "";
			SavePadState(padName);
		}
		refreshed();
		return done(true, "");
	}

	// ---- Device ops. ----

	// Release mode: 0 = none, 2 = individual (ReleaseMode enum). Global (1) has
	// been removed, so it is refused like any other unknown value.
	if (opName == "setReleaseMode")
	{
		const int64_t mode = j["releaseMode"].is_number_integer() ? j["releaseMode"].get<int64_t>() : -1;
		if (mode != RELEASE_NONE && mode != RELEASE_INDIVIDUAL)
			return done(false, "unknown release mode");
		return device->SetReleaseMode((ReleaseMode)mode) ? done(true, "") : done(false, "the pad didn't accept it");
	}

	// Names a button for display: {pad, buttonName:{button, name}}, a blank name
	// clears it. Kept in the pad's state, so it follows pad renames.
	if (opName == "renameButton")
	{
		const json& b = j["buttonName"];
		if (!b.is_object() || !b.contains("button") || !b["button"].is_number_integer() || !HasString(b, "name"))
			return done(false, "needs a button and a name");
		const int64_t button = b["button"].get<int64_t>();
		if (button < 1 || button > device->State().numButtons)
			return done(false, "unknown button");
		if (OtherPadNamed(device, padName))
			return done(false, fmt::format("another pad here is also named '{}', rename one first", padName));
		std::string name = Profiles::ValidUtf8(b["name"].get<std::string>());
		name.erase(0, name.find_first_not_of(" \t"));
		name.erase(name.find_last_not_of(" \t") + 1);
		if (std::count_if(name.begin(), name.end(), [](char c) { return (c & 0xC0) != 0x80; }) > MAX_BUTTON_NAME)
			return done(false, fmt::format("the name is longer than {} characters", MAX_BUTTON_NAME));
		json& st = PadStateFor(padName);
		if (!st.contains("buttonNames") || !st["buttonNames"].is_object())
			st["buttonNames"] = json::object();
		if (name.empty())
			st["buttonNames"].erase(std::to_string(button));
		else
			st["buttonNames"][std::to_string(button)] = name;
		SavePadState(padName);
		return done(true, "", name);
	}

	// A rename. Profiles and pad state are keyed by pad name and move along.
	if (opName == "rename")
	{
		if (!HasString(j, "name"))
			return done(false, "needs a name");
		const std::string newName = j["name"].get<std::string>();
		if (Profiles::Blank(newName))
			return done(false, "the name must not be empty");
		if (OtherPadNamed(device, newName))
			return done(false, fmt::format("another pad here is already named '{}'", newName));
		if (const std::string conflict = RenameConflict(padName, newName); !conflict.empty())
			return done(false, conflict);
		if (!device->SendName(newName.c_str()))
			return done(false, "the pad didn't accept the name");
		// Into the EEPROM before the profiles move: a pad unplugged before its
		// usual save (~2s later) would come back with the old name, its
		// profiles filed under the new one.
		if (!device->SaveChanges())
			std::printf("rename :: the pad didn't save its new name yet (retried in 2s)\n");
		RenamePad(padName, device->State().name);
		return done(true, "", device->State().name);
	}

	// A configuration update ({sensors} and/or {lightRules, ledMappings})
	// reuses the hardened LoadProfile path, which validates every index and
	// group before touching device state. Sent at slider-drag rate, so only a
	// failure is answered.
	if (opName == "configure")
	{
		if (j.contains("sensors") && (!j["sensors"].is_array() || (int)j["sensors"].size() != numSensors))
			return done(false, fmt::format("expected {} sensors", numSensors));
		if (const std::string why = device->UnlitMapping(j); !why.empty())
			return done(false, why);
		std::vector<std::string> failed;
		if (!device->LoadProfile(j, (DeviceProfileGroups)(DPG_ALL & ~DPG_DEVICE), &failed))
			return done(false, NotApplied(failed));
		SetEditsLost(padName, false);
		// Answered either way; the caller passes a success on only to askers
		// that sent a requestId (slider rate).
		return done(true, "");
	}

	std::printf("HandleClientMessage :: unhandled message: %s\n", j.dump().c_str());
	return replies;
}

void Device::AddPadForTest(std::unique_ptr<PadDevice> pad)
{
	if (!connectionManager)
		connectionManager = std::make_unique<ConnectionManager>();
	connectionManager->AddPad(std::move(pad));
	gPadStatesDirty = true;
}

std::set<std::string> Device::BusyPads()
{
	std::set<std::string> busy;
	for (PadDevice* device : connectionManager ? connectionManager->Pads() : vector<PadDevice*>{})
		if (device->Saving())
			busy.insert(device->Path());
	return busy;
}

bool Device::BackgroundWorkForTest()
{
	return connectionManager && connectionManager->BackgroundWork();
}

void Device::ResetForTest()
{
	connectionManager.reset();
	gPadStates.clear();
	gPadStatesDirty = true;
	gLightsDirty = false;
}

// ---- Lights from a client or profile -------------------------------------------------------------------------------

// A whole number 0..255 at `key` (absent: 0), as the report's byte fields hold.
static bool ReadByteField(const json& slot, const char* key, int& out)
{
	out = 0;
	if (!slot.contains(key))
		return true;
	const json& v = slot[key];
	if (!v.is_number_integer())
		return false;
	const auto n = v.get<int64_t>();
	if (n < 0 || n > 255)
		return false;
	out = (int)n;
	return true;
}

static bool ReadBoolField(const json& slot, const char* key, bool& out)
{
	out = false;
	if (!slot.contains(key))
		return true;
	if (!slot[key].is_boolean())
		return false;
	out = slot[key].get<bool>();
	return true;
}

static bool ReadColorField(const json& slot, const char* key, RgbColor& out)
{
	out = RgbColor();
	if (!slot.contains(key))
		return true;
	if (!slot[key].is_string())
		return false;
	out = RgbColor(slot[key].get<std::string>());
	return true;
}

static bool ParseLedMapping(const json& slot, LedMapping& m)
{
	return ReadByteField(slot, "lightRuleIndex", m.lightRuleIndex) &&
	       ReadByteField(slot, "sensorIndex", m.sensorIndex) && ReadByteField(slot, "ledIndexBegin", m.ledIndexBegin) &&
	       ReadByteField(slot, "ledIndexEnd", m.ledIndexEnd);
}

static bool ParseLightRule(const json& slot, LightRule& r)
{
	return ReadBoolField(slot, "fadeOn", r.fadeOn) && ReadBoolField(slot, "fadeOff", r.fadeOff) &&
	       ReadColorField(slot, "onColor", r.onColor) && ReadColorField(slot, "offColor", r.offColor) &&
	       ReadColorField(slot, "onFadeColor", r.onFadeColor) && ReadColorField(slot, "offFadeColor", r.offFadeColor);
}

// Slot i of a lights array configures slot i on the pad; a slot that is
// missing or not an object (profiles from pads with gaps between enabled slots
// carry nulls there) is disabled (nullopt). False if any slot is malformed or
// the array has more slots than a pad.
template <typename T, typename Parse>
static bool ParseSlots(const json& arr, int max, std::vector<std::optional<T>>& out, Parse parse)
{
	if ((int)arr.size() > max)
		return false;
	out.assign(max, std::nullopt);
	for (int i = 0; i < (int)arr.size(); ++i)
	{
		if (!arr[i].is_object())
			continue;
		T v{};
		if (!parse(arr[i], v))
			return false;
		out[i] = v;
	}
	return true;
}

void Device::DiscoverNewDevices()
{
	// Pads added or removed.
	if (connectionManager->DiscoverDevice())
	{
		gLightsDirty = true;
		gPadStatesDirty = true;
	}
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
		d["firmware"] =
		    fmt::format("{}.{}", device->State().firmwareVersion.major, device->State().firmwareVersion.minor);
		d["maxSensorValue"] = device->State().maxSensorValue;
		d["numButtons"] = device->State().numButtons;
		d["featureDigipot"] = device->State().featureDigipot;
		d["featureLights"] = device->State().featureLights;
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

// A whole number at `key` within [lo, hi], or nullopt if absent or not one
// (a client's 4294967297 must not wrap to 1).
static std::optional<int> IntField(const json& o, const char* key, int64_t lo, int64_t hi)
{
	if (!o.contains(key) || !o[key].is_number_integer())
		return std::nullopt;
	const json& v = o[key];
	if (v.is_number_unsigned() ? v.get<uint64_t>() > (uint64_t)hi : v.get<int64_t>() > hi)
		return std::nullopt;
	if (!v.is_number_unsigned() && v.get<int64_t>() < lo)
		return std::nullopt;
	return (int)v.get<int64_t>();
}

bool PadDevice::LoadProfile(const json& j, DeviceProfileGroups groups, std::vector<std::string>* failed)
{
	const PadState* pad = &myPad;
	bool ok = true;
	const auto fail = [&](const std::string& what) {
		ok = false;
		if (failed && std::find(failed->begin(), failed->end(), what) == failed->end())
			failed->push_back(what);
	};

	if ((groups & DPG_LIGHTS) && pad->featureLights)
	{
		// Every slot is parsed and checked before anything is sent, so a bad one
		// can't leave the lights half applied. Only slots that differ from the
		// pad are sent: each report is a HID round-trip that stalls polling of
		// every pad, and a full rewrite is 32 of them.
		std::vector<std::optional<LedMapping>> mappings;
		std::vector<std::optional<LightRule>> rules;
		bool sent = false;
		std::string why;
		if (!ParseLights(j, mappings, rules, &why))
			fail("lights: " + why);
		for (int i = 0; i < (int)mappings.size(); ++i)
		{
			const auto current = myLights.ledMappings.find(i);
			const bool enabled = current != myLights.ledMappings.end();
			if (!mappings[i] ? enabled : !enabled || !(current->second == *mappings[i]))
			{
				if (!(mappings[i] ? SendLedMapping(i, *mappings[i]) : DisableLedMapping(i)))
					fail(fmt::format("LED mapping {}", i));
				sent = true;
			}
		}
		for (int i = 0; i < (int)rules.size(); ++i)
		{
			const auto current = myLights.lightRules.find(i);
			const bool enabled = current != myLights.lightRules.end();
			if (!rules[i] ? enabled : !enabled || !(current->second == *rules[i]))
			{
				if (!(rules[i] ? SendLightRule(i, *rules[i]) : DisableLightRule(i)))
					fail(fmt::format("light rule {}", i));
				sent = true;
			}
		}
		// Only then are the published lights stale.
		if (sent)
			TriggerChange(DCF_LIGHTS);
	}

	if (j.contains("sensors") && j["sensors"].is_array())
	{
		const auto& sensors = j["sensors"];
		// Entries past this pad's sensor count are ignored (an imported profile
		// only has its sensorCount checked, not its array length).
		for (size_t key = 0; key < sensors.size() && ValidSensorIndex((int)key); ++key)
		{
			const auto& sensor = sensors[key];
			const int idx = (int)key;

			// Apply whichever fields the entry carries on top of the current values
			// (a release-only patch from the client's release slider must still
			// reach the device), then send them all in one report, or none if
			// nothing changed.
			SensorState want = mySensors[idx];
			if (groups & DPG_SENSITIVITY)
			{
				if (sensor.contains("threshold") && sensor["threshold"].is_number())
					want.threshold = sensor["threshold"].get<double>();
				if (sensor.contains("releaseThreshold") && sensor["releaseThreshold"].is_number())
					want.releaseThreshold = sensor["releaseThreshold"].get<double>();
			}
			if ((groups & DPG_MAPPING) && sensor.contains("button"))
			{
				if (const auto button = IntField(sensor, "button", 0, myPad.numButtons))
					want.button = *button;
				else
					fail(fmt::format("sensor {} button", idx));
			}
			if ((groups & DPG_GAIN) && pad->featureDigipot && sensor.contains("resistorValue"))
			{
				if (const auto gain = IntField(sensor, "resistorValue", 0, 255))
					want.resistorValue = *gain;
				else
					fail(fmt::format("sensor {} gain", idx));
			}
			if (!SetSensor(idx, want))
				fail(fmt::format("sensor {}", idx));
		}
	}

	if (groups & DPG_DEVICE)
	{
		if (j.contains("name") && j["name"].is_string())
		{
			if (!SendName(j["name"].get<string>().c_str()))
				fail("name");
		}
	}
	return ok;
}

std::string PadDevice::UnlitMapping(const json& j) const
{
	std::vector<std::optional<LedMapping>> mappings;
	std::vector<std::optional<LightRule>> rules;
	if (!myPad.featureLights || !ParseLights(j, mappings, rules))
		return ""; // nothing to light, or refused by LoadProfile anyway
	const bool hasRules = j.contains("lightRules") && j["lightRules"].is_array();
	for (int i = 0; i < (int)mappings.size(); ++i)
	{
		if (!mappings[i])
			continue;
		// One the pad already has stays allowed, so its other lights can still be edited.
		const auto current = myLights.ledMappings.find(i);
		if (current != myLights.ledMappings.end() && current->second == *mappings[i])
			continue;
		const int rule = mappings[i]->lightRuleIndex; // < MAX_LIGHT_RULES (ValidLedMapping)
		if (!(hasRules ? rules[rule].has_value() : myLights.lightRules.contains(rule)))
			return fmt::format("LED mapping {} uses light rule {}, which is disabled", i, rule);
	}
	return "";
}

bool PadDevice::ParseLights(const json& j, std::vector<std::optional<LedMapping>>& mappings,
                            std::vector<std::optional<LightRule>>& rules, std::string* why) const
{
	const bool hasMappings = j.contains("ledMappings") && j["ledMappings"].is_array();
	const bool hasRules = j.contains("lightRules") && j["lightRules"].is_array();
	const auto refuse = [&](const char* reason) {
		std::printf("LoadProfile :: %s, no lights applied\n", reason);
		if (why)
			*why = reason;
		mappings.clear();
		rules.clear();
		return false;
	};
	mappings.clear();
	rules.clear();
	if ((hasMappings && !ParseSlots(j["ledMappings"], MAX_LED_MAPPINGS, mappings, ParseLedMapping)) ||
	    (hasRules && !ParseSlots(j["lightRules"], MAX_LIGHT_RULES, rules, ParseLightRule)))
		return refuse("malformed lights");
	if (!std::all_of(mappings.begin(), mappings.end(),
	                 [&](const std::optional<LedMapping>& m) { return !m || ValidLedMapping(*m); }))
		return refuse("LED mapping outside this pad's sensors, rules or LEDs");
	return true;
}

void PadDevice::SaveProfile(json& j, DeviceProfileGroups groups) const
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
	}

	if (groups & DPG_DEVICE)
	{
		j["name"] = pad->name;
	}
}

} // namespace adp
