#pragma once

#include "stdint.h"
#include <string>
#include <map>
#include <memory>
#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

#include <Model/Reporter.h>

namespace adp
{
using json = nlohmann::json;

class PadDevice;

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
	DPG_MAPPING = 1 << 1, // button mappings only
	DPG_DEVICE = 1 << 2,
	DPG_LIGHTS = 1 << 3,
	DPG_GAIN = 1 << 4, // per-sensor digipot gain (resistorValue), split out of DPG_MAPPING

	DGP_ALL = 0b1111111111111111
};

typedef int32_t DeviceProfileGroups;

struct RgbColor
{
	RgbColor(uint8_t r, uint8_t g, uint8_t b);
	RgbColor(const std::string& input);
	RgbColor();

	std::string ToString() const;
	bool operator==(const RgbColor&) const = default;

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
	int flags = 0; // SensorReport flags (e.g. ADC_DISABLED): device-owned, written back unchanged

	SensorReport ToReport(int index);
};

struct LedMapping
{
	int lightRuleIndex;
	int sensorIndex;
	int ledIndexBegin;
	int ledIndexEnd;

	bool operator==(const LedMapping&) const = default;
};

struct LightRule
{
	bool fadeOn;
	bool fadeOff;
	RgbColor onColor;
	RgbColor offColor;
	RgbColor onFadeColor;
	RgbColor offFadeColor;

	bool operator==(const LightRule&) const = default;
};

struct LightsState
{
	std::map<int, LightRule> lightRules;
	std::map<int, LedMapping> ledMappings;
};

// An immutable, self-contained copy of everything the WebSocket clients need to
// see about one connected pad. Built on the device-I/O thread each
// tick and published via an atomic shared_ptr, so readers never touch live
// device state (no locks, no torn reads, no TOCTOU null-deref).
struct SensorSnapshot
{
	bool connected = false;
	std::string path; // HID path: how clients address this pad in messages
	std::string name;
	int pollingRate = 0;
	int releaseMode = 0;
	int numButtons = 0;
	bool featureDigipot = false;
	bool featureLights = false;
	bool unsaved = false;    // edits not yet in the pad's EEPROM (saved ~2s after the last one)
	bool saveFailed = false; // the last save attempt failed; retried every 2s
	std::vector<SensorState> sensors;
};

struct VersionType
{
	uint16_t major = 0;
	uint16_t minor = 0;
};

struct PadState
{
	std::string name;
	int maxNameLength = 0;
	int numButtons = 0;
	int numSensors = 0;
	std::string boardType; // e.g. "avr_fsrio1", as the firmware reports it
	bool featureDigipot = false;
	bool featureLights = false;
	VersionType firmwareVersion;
	ReleaseMode releaseMode = ReleaseMode::RELEASE_INDIVIDUAL;
};

// One reply to an inbound client message: to the client that sent it, or
// broadcast to every client.
struct ClientReply
{
	json message;
	bool toSender = true;
};

class Device
{
  public:
	static void Init();

	static void Shutdown();

	// Polls every pad, then publishes their snapshots. Device-I/O thread only.
	static void Update();

	// Builds an immutable snapshot of every connected pad and publishes them
	// atomically. Called on the device-I/O thread (from Update()).
	static void PublishSnapshot();

	// Returns the most recently published snapshots, one per pad (may be null
	// before the first tick). Safe to call from any thread.
	static std::shared_ptr<const std::vector<SensorSnapshot>> GetSnapshots();

	// Serializes a snapshot to the client-facing JSON wire format.
	static void SnapshotToJson(const SensorSnapshot& snapshot, json& j);

	// Serializes the list of connected pads (msgType 2) so clients can
	// enumerate them and address messages by path.
	static void DeviceListToJson(json& j);

	// Serializes one pad's lights (msgType 4). Broadcast only on
	// change (see TakeLightsDirty), not with every snapshot.
	static void LightsToJson(const std::string& pad, const LightsState& lights, json& j);

	// One lights message per connected pad, read straight from the pads.
	// Device-I/O thread only.
	static std::vector<std::string> LightsMessages();

	// Returns true and clears the flag if the pad's lights changed (or a device
	// was (re)selected) since the last call, so the caller can rebroadcast them.
	static bool TakeLightsDirty();

	// Parses and dispatches one inbound client message. Must be called on the
	// device-I/O thread so device access stays single-threaded. Returns the
	// replies to send.
	static std::vector<ClientReply> HandleClientMessage(const std::string& message);

	// Serializes each pad's server-owned selection (msgType 5): current player
	// and profile plus recent players, keyed by pad path.
	static void PadStatesToJson(json& j);

	// Returns true and clears the flag if any pad state changed since the last call.
	static bool TakePadStatesDirty();

	// Root of this server's local profile store. Set once at startup.
	static void SetProfilesDir(const std::filesystem::path& dir);

	static void DiscoverNewDevices();

	// Tests only: put a pad (on a fake backend) in the device map, and drop all
	// pads and cached pad states again. No HID access.
	static void AddPadForTest(std::unique_ptr<PadDevice> pad);
	static void ResetForTest();
};

} // namespace adp
