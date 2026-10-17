#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The player list, read from ITGmania's LocalProfiles folder on a network
// share: every subfolder holds an Editable.ini with a "DisplayName=..." line
// and optionally an avatar.<ext>. A share can be slow, so scanning runs on its
// own thread and publishes an immutable list (never on the device thread).
// Profiles themselves live in each server's local profile store.
//
// The share may be unmounted at any time. Then the last known list is kept
// (flagged localOk=false) and the scanner retries every few seconds, so
// everything recovers as soon as it is mounted again.
//
// Players are keyed by DisplayName; two folders with the same name (ignoring
// case: on Windows they would share a profile dir anyway) collapse to one
// entry and share profiles. When a folder's name changes between scans the
// player's profile dir is renamed along with it (see PlanRenames).
namespace adp::Players
{
using json = nlohmann::json;
using Path = std::filesystem::path;

struct Player
{
	std::string name;                         // DisplayName, the profile key
	std::string folder;                       // LocalProfiles subfolder, e.g. "00000003"
	Path avatar;                              // empty if the folder has none
	std::map<std::string, int> profileCounts; // pad name -> stored profiles for it
};

struct Rename
{
	std::string from, to;
	std::string folder; // the LocalProfiles folder whose DisplayName changed
};

struct PlayerList
{
	std::vector<Player> players; // sorted by name, deduplicated
	bool localOk = true;         // LocalProfiles was reachable on the last scan (else players are stale)
	std::vector<Rename> renamed; // players this scan followed to a new name (their profiles moved along)
};

// DisplayName from Editable.ini contents ("" if missing). Tolerates a UTF-8
// BOM, CRLF line endings and surrounding whitespace.
std::string ParseDisplayName(const std::string& ini);

// MIME type if `filename` is "avatar.<image/video ext>" (any case), else "".
std::string AvatarMime(const std::string& filename);

// Profile dirs to rename, given the previous and current folder -> name maps.
// A rename is skipped while another folder still uses the old name, so one of
// two same-named players renaming never carries off the other's profiles.
std::vector<Rename> PlanRenames(const std::map<std::string, std::string>& before,
                                const std::map<std::string, std::string>& now);

std::string Base64(const std::string& bytes);

// One full scan: reads LocalProfiles, follows renames, counts stored profiles.
// An unreachable LocalProfiles falls back to `previous.players`.
// May throw std::filesystem::filesystem_error if the share drops mid-scan.
// With `hold`, the store lock taken for the renames is handed over in it
// instead of released, so the caller can publish the renamed list before any
// profile op sees the old names gone (an op on an old name would recreate its dir).
PlayerList Scan(const Path& localProfilesDir, const Path& profilesDir, const PlayerList& previous = {},
                std::unique_lock<std::mutex>* hold = nullptr);

// msgType 3 for clients.
json ToJson(const PlayerList& list);

// Background scanner: rescans every `interval` (every few seconds while
// LocalProfiles is unreachable), or sooner on RequestRescan().
void Start(const Path& localProfilesDir, const Path& profilesDir, std::chrono::seconds interval);
void Stop();
void RequestRescan();
// Recounts the stored profiles (local disk) without rescanning LocalProfiles
// (the share): of one player after their profile op, of everyone ("") after
// a pad rename.
void RequestRecount(const std::string& player = "");

// `previous` with the profile counts of the players in `only` (every player
// if null) read again from `profilesDir`.
PlayerList Recount(const Path& profilesDir, const PlayerList& previous, std::unique_lock<std::mutex>* hold = nullptr,
                   const std::set<std::string>* only = nullptr);

// Renames followed since the last call, for the pad selections to follow
// too. Thread-safe.
std::vector<Rename> TakeRenames();

// Tests only: publish `list` (and its renames) as if a scan had found it.
void SetForTest(PlayerList list);

// Latest published list (empty before the first scan). Thread-safe.
std::shared_ptr<const PlayerList> Get();

// Bumped when a scan finds a different list, so the broadcaster knows to resend.
uint64_t Version();

// True if `name` is a known player. Client-supplied names are checked with
// this before any path is built from them.
bool Has(const std::string& name);

// msgType 7 {name, mime, data(base64)} for one player's avatar. mime/data are
// empty if there is none or it exceeds the size cap. Cached per file (until it
// changes), so repeated requests don't re-read the share. Thread-safe.
std::string AvatarMessage(const std::string& name);
} // namespace adp::Players
