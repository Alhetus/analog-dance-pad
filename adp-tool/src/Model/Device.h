#pragma once

#include "stdint.h"
#include <string>
#include <map>
#include <memory>
#include <filesystem>
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
	std::vector<SensorState> sensors;
	LightsState lights; // for client-side profile drift detection
};

struct VersionType
{
	uint16_t major;
	uint16_t minor;

	bool IsNewer(VersionType then) const
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
	// change / periodically (see TakeLightsDirty), not with every snapshot.
	static void LightsToJson(const SensorSnapshot& snapshot, json& j);

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
};

} // namespace adp
