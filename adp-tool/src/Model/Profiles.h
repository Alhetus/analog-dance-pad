#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

// Profile storage: each profile is one JSON file (<id>.json) in a directory.
// The directory is <profilesDir>/players/<player>/<pad>/ (see PlayerPadDir),
// on the server's local disk: each server stores the profiles of its own pads.
// Pure filesystem I/O, no device/HID access, so it is unit-testable on its own.
//
// `id` is the filename stem and the trust boundary: it comes from untrusted
// WebSocket clients and is turned into a path, so every function validates it
// with ValidId() and refuses anything outside [a-z0-9-] (no traversal).
namespace adp::Profiles
{
using json = nlohmann::json;
using Path = std::filesystem::path;

// Most profiles a player may keep per pad.
constexpr size_t MAX_PER_PAD = 5;

// Longest profile name and description stored, in UTF-8 bytes.
constexpr size_t MAX_NAME_BYTES = 64;
constexpr size_t MAX_DESCRIPTION_BYTES = 1000;

// A path from a UTF-8 string. std::filesystem::path(std::string) uses the ANSI
// codepage on Windows and would mangle names like "Åsa", so always go via u8.
Path U8Path(const std::string& utf8);

// The UTF-8 string form of a path (inverse of U8Path).
std::string PathU8(const Path& p);

// Turns an arbitrary UTF-8 display name (player or pad) into one safe path
// component: percent-encodes path separators, Windows-illegal chars, control
// chars, '%', a leading/trailing '.' or trailing space, and the first char of
// Windows reserved names (CON, NUL, COM1...). Empty input yields "".
std::string EscapeName(const std::string& name);

// Inverse of EscapeName.
std::string UnescapeName(const std::string& escaped);

// `s` with every byte that isn't part of a valid UTF-8 sequence replaced by
// U+FFFD. Names read from a pad or an Editable.ini are raw bytes, and json
// refuses to serialize invalid UTF-8 (it throws), so they pass through this.
std::string ValidUtf8(const std::string& s);

// <profilesDir>/players/<esc(player)>/<esc(pad)>
Path PlayerPadDir(const Path& profilesDir, const std::string& player, const std::string& pad);

// True for a safe filename stem: non-empty, only lowercase letters/digits/'-'.
bool ValidId(const std::string& id);

// Turns an arbitrary display name into a valid id (never empty).
std::string SlugifyId(const std::string& name);

// True if <dir>/<id>.json exists (false for an invalid id).
bool Exists(const Path& dir, const std::string& id);

// Every stored profile as a JSON array (unparseable files are skipped). Each
// profile's "id" is its filename stem, whatever the file itself says.
json List(const Path& dir);

// Reads one JSON file into `out`; false if missing, invalid id, or bad JSON.
bool Read(const Path& dir, const std::string& id, json& out);

// Read() for a profile: also false unless it is an object, and "id" is set to
// `id` (a hand-edited or copied file may carry none, or a stale one).
bool ReadProfile(const Path& dir, const std::string& id, json& out);

// Atomically and durably writes (temp file, fsync, rename): cabs get
// power-cut. False on invalid id or I/O error.
bool Write(const Path& dir, const std::string& id, const json& profile);

// Deletes one profile; false on invalid id. True even if it was already gone.
bool Delete(const Path& dir, const std::string& id);

// Held while the store's directory layout may change (player/pad dir renames)
// and around profile reads/writes, so neither sees the other half-done.
std::mutex& StoreMutex();

// The storable settings of a client-supplied profile (importProfile), rebuilt
// field by field: only known fields of the expected type are kept (sensor
// thresholds clamped to 0..1, gain and light indices to one byte, colors as
// lowercase #rrggbb, the shape a live pad reports). Returns null unless it has
// a sensorCount and a sensors array of that length, its lights arrays fit a
// pad's slots, and its description fits MAX_DESCRIPTION_BYTES. id/name/savedAt
// are set by the caller.
json Sanitize(const json& profile);
} // namespace adp::Profiles
