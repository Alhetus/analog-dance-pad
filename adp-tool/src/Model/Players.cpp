#include <Model/Players.h>
#include <Model/Profiles.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace adp::Players
{
static std::string Trim(const std::string& s)
{
	const char* ws = " \t\r\n";
	const size_t b = s.find_first_not_of(ws);
	if (b == std::string::npos)
		return "";
	return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

std::string ParseDisplayName(const std::string& ini)
{
	std::string text = ini;
	if (text.rfind("\xEF\xBB\xBF", 0) == 0)
		text.erase(0, 3);

	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line))
	{
		line = Trim(line);
		if (line.rfind("DisplayName=", 0) == 0)
			return Trim(line.substr(12));
	}
	return "";
}

static std::string Lower(std::string s)
{
	for (auto& c : s)
		c = (char)std::tolower((unsigned char)c);
	return s;
}

std::string AvatarMime(const std::string& filename)
{
	const std::string lower = Lower(filename);
	if (lower.rfind("avatar.", 0) != 0)
		return "";

	static const std::map<std::string, std::string> types = {
	    {"png", "image/png"},   {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},    {"gif", "image/gif"},
	    {"webp", "image/webp"}, {"bmp", "image/bmp"},  {"avif", "image/avif"},    {"mp4", "video/mp4"},
	    {"webm", "video/webm"}, {"m4v", "video/mp4"},  {"mov", "video/quicktime"}};
	auto it = types.find(lower.substr(7));
	return it == types.end() ? "" : it->second;
}

std::vector<Rename> PlanRenames(const std::map<std::string, std::string>& before,
                                const std::map<std::string, std::string>& now)
{
	std::set<std::string> namesNow;
	for (const auto& [folder, name] : now)
		namesNow.insert(name);

	std::vector<Rename> out;
	for (const auto& [folder, name] : now)
	{
		auto it = before.find(folder);
		if (it == before.end() || it->second == name || namesNow.contains(it->second))
			continue;
		out.push_back({it->second, name, folder});
	}
	return out;
}

std::string Base64(const std::string& bytes)
{
	static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((bytes.size() + 2) / 3 * 4);
	size_t i = 0;
	for (; i + 2 < bytes.size(); i += 3)
	{
		const uint32_t n = ((uint8_t)bytes[i] << 16) | ((uint8_t)bytes[i + 1] << 8) | (uint8_t)bytes[i + 2];
		out += {t[n >> 18], t[(n >> 12) & 63], t[(n >> 6) & 63], t[n & 63]};
	}
	if (i + 1 == bytes.size())
	{
		const uint32_t n = (uint8_t)bytes[i] << 16;
		out += {t[n >> 18], t[(n >> 12) & 63], '=', '='};
	}
	else if (i + 2 == bytes.size())
	{
		const uint32_t n = ((uint8_t)bytes[i] << 16) | ((uint8_t)bytes[i + 1] << 8);
		out += {t[n >> 18], t[(n >> 12) & 63], t[(n >> 6) & 63], '='};
	}
	return out;
}

static bool ReadFile(const Path& p, std::string& out, size_t maxBytes)
{
	std::error_code ec;
	const auto size = fs::file_size(p, ec);
	if (ec || size > maxBytes)
		return false;
	std::ifstream in(p, std::ios::binary);
	if (!in)
		return false;
	out.assign(std::istreambuf_iterator<char>(in), {});
	return true;
}

// Renames players/<from> -> players/<to> in this server's local profile store
// where it is safe, and remembers the folder -> name map next to it, so a
// rename that happened while this server was off is still followed.
static void FollowRenames(const Path& profilesDir, const std::map<std::string, std::string>& now)
{
	json before;
	Profiles::Read(profilesDir, "player-names", before);
	std::map<std::string, std::string> prev;
	if (before.is_object())
		for (auto& [folder, name] : before.items())
			if (name.is_string())
				prev[folder] = name.get<std::string>();

	// What gets remembered: the new names, except where the profiles could not
	// be moved; those keep the old name so the move is retried next scan.
	std::map<std::string, std::string> remembered = now;

	std::error_code ec;
	for (const auto& r : PlanRenames(prev, now))
	{
		const Path from = profilesDir / "players" / Profiles::U8Path(Profiles::EscapeName(r.from));
		const Path to = profilesDir / "players" / Profiles::U8Path(Profiles::EscapeName(r.to));
		if (!fs::is_directory(from, ec))
			continue; // no profiles to move
		if (fs::exists(to, ec))
		{
			// ponytail: no merging; both dirs are kept and the move retried each scan.
			remembered[r.folder] = r.from;
			continue;
		}
		fs::rename(from, to, ec);
		std::printf("Players :: '%s' renamed to '%s', profiles %s\n", r.from.c_str(), r.to.c_str(),
		            ec ? "NOT moved, will retry" : "moved");
		if (ec)
			remembered[r.folder] = r.from;
	}

	// An empty map is never a real venue state; skip it so a scan that read
	// nothing (share flaking mid-scan) can't wipe the remembered names.
	if (prev != remembered && !remembered.empty())
		Profiles::Write(profilesDir, "player-names", json(remembered));
}

static std::map<std::string, int> CountProfiles(const Path& playerDir)
{
	std::map<std::string, int> counts;
	std::error_code ec;
	if (!fs::is_directory(playerDir, ec))
		return counts;
	for (const auto& pad : fs::directory_iterator(playerDir, ec))
	{
		if (!pad.is_directory(ec))
			continue;
		int n = 0;
		for (const auto& f : fs::directory_iterator(pad.path(), ec))
			n += f.path().extension() == ".json";
		if (n > 0)
			counts[Profiles::UnescapeName(Profiles::PathU8(pad.path().filename()))] = n;
	}
	return counts;
}

PlayerList Scan(const Path& localProfilesDir, const Path& profilesDir, const PlayerList& previous)
{
	PlayerList list;
	std::error_code ec;
	if (localProfilesDir.empty())
		return list; // not configured: no players, nothing to warn about
	if (!fs::is_directory(localProfilesDir, ec))
	{
		list.players = previous.players; // unmounted: keep the last known players
		list.localOk = false;
		return list;
	}

	std::vector<Path> folders;
	for (const auto& e : fs::directory_iterator(localProfilesDir, ec))
		if (e.is_directory(ec))
			folders.push_back(e.path());
	std::sort(folders.begin(), folders.end()); // deterministic winner for duplicate names

	// On Linux an unmounted share usually leaves an empty mount point behind
	// rather than a missing dir. A real LocalProfiles is never empty, so treat
	// "no player folders at all" as unreachable too and keep the last list.
	if (folders.empty())
	{
		list.players = previous.players;
		list.localOk = false;
		return list;
	}

	std::map<std::string, std::string> folderNames;
	std::map<std::string, Player> byName;
	for (const auto& folder : folders)
	{
		// Match file names case-insensitively: the share may have been written
		// from Windows, while Linux paths are case-sensitive.
		Path editable, avatar;
		for (const auto& f : fs::directory_iterator(folder, ec))
		{
			if (!f.is_regular_file(ec))
				continue;
			const std::string fileName = Profiles::PathU8(f.path().filename());
			if (Lower(fileName) == "editable.ini")
				editable = f.path();
			else if (avatar.empty() && !AvatarMime(fileName).empty())
				avatar = f.path();
		}

		std::string ini;
		if (editable.empty() || !ReadFile(editable, ini, 64 * 1024))
			continue;
		const std::string name = ParseDisplayName(ini);
		if (name.empty())
			continue;

		const std::string folderName = Profiles::PathU8(folder.filename());
		folderNames[folderName] = name;
		if (byName.contains(name))
		{
			std::printf("Players :: duplicate DisplayName '%s' in %s and %s; they share profiles\n", name.c_str(),
			            byName[name].folder.c_str(), folderName.c_str());
			continue;
		}

		Player p;
		p.name = name;
		p.folder = folderName;
		p.avatar = avatar;
		byName[name] = std::move(p);
	}

	// The share vanished during the scan: what we read may be partial.
	if (!fs::is_directory(localProfilesDir, ec))
	{
		list.players = previous.players;
		list.localOk = false;
		return list;
	}

	if (!profilesDir.empty())
	{
		// Local disk only (the share was read above), so this is held briefly.
		std::lock_guard<std::mutex> lock(Profiles::StoreMutex());
		FollowRenames(profilesDir, folderNames);
		for (auto& [name, p] : byName)
			p.profileCounts = CountProfiles(profilesDir / "players" / Profiles::U8Path(Profiles::EscapeName(name)));
	}

	for (auto& [name, p] : byName)
		list.players.push_back(std::move(p));
	return list;
}

PlayerList Recount(const Path& profilesDir, const PlayerList& previous)
{
	PlayerList list = previous;
	if (profilesDir.empty())
		return list;
	std::lock_guard<std::mutex> lock(Profiles::StoreMutex());
	for (auto& p : list.players)
		p.profileCounts = CountProfiles(profilesDir / "players" / Profiles::U8Path(Profiles::EscapeName(p.name)));
	return list;
}

json ToJson(const PlayerList& list)
{
	json j;
	j["msgType"] = 3;
	j["localProfiles"] = list.localOk; // false: players are the last known, share unreachable
	j["players"] = json::array();
	for (const auto& p : list.players)
		j["players"].push_back({{"name", p.name}, {"avatar", !p.avatar.empty()}, {"profiles", p.profileCounts}});
	return j;
}

// ---- Background scanner --------------------------------------------------------------------------------------------

static std::mutex gMutex;
static std::condition_variable gCv;
static std::shared_ptr<const PlayerList> gList = std::make_shared<PlayerList>();
static std::atomic<uint64_t> gVersion{0};
static bool gStop = false;
static bool gRescan = false;
static bool gRecount = false;
static std::thread gThread;

void Start(const Path& localProfilesDir, const Path& profilesDir, std::chrono::seconds interval)
{
	if (localProfilesDir.empty())
		std::printf("Players :: no --local-profiles-dir given, player list is empty\n");
	else
		std::printf("LocalProfiles directory: %s\n", Profiles::PathU8(localProfilesDir).c_str());

	gThread = std::thread([=] {
		constexpr auto retryInterval = std::chrono::seconds(5); // while LocalProfiles is unreachable
		std::unique_lock lock(gMutex);
		// This thread is the only writer of gList, so a recount can't be
		// overwritten by the counts of a scan that started before it.
		bool recountOnly = false;
		while (!gStop)
		{
			const auto prev = gList;
			lock.unlock();
			PlayerList next;
			try
			{
				next = recountOnly ? Recount(profilesDir, *prev) : Scan(localProfilesDir, profilesDir, *prev);
			}
			catch (const std::exception& e)
			{
				// The share dropped mid-scan (directory iteration throws).
				std::printf("Players :: scan failed (%s), keeping last known players\n", e.what());
				next = *prev;
				next.localOk = false;
			}
			if (next.localOk != prev->localOk || gVersion == 0)
				std::printf("Players :: LocalProfiles %s\n", next.localOk ? "reachable" : "UNREACHABLE, retrying");
			const bool healthy = next.localOk;
			// Bump the version only on a real change, so clients aren't resent
			// (and don't re-render) an identical list after every scan.
			const bool changed = gVersion == 0 || ToJson(next) != ToJson(*prev);
			lock.lock();
			gList = std::make_shared<const PlayerList>(std::move(next));
			if (changed)
				gVersion++;
			gCv.wait_for(lock, healthy ? interval : retryInterval, [] { return gStop || gRescan || gRecount; });
			recountOnly = gRecount && !gRescan; // a full scan recounts too
			gRescan = gRecount = false;
		}
	});
}

void Stop()
{
	{
		std::lock_guard lock(gMutex);
		gStop = true;
	}
	gCv.notify_all();
	if (gThread.joinable())
		gThread.join();
}

void RequestRescan()
{
	{
		std::lock_guard lock(gMutex);
		gRescan = true;
	}
	gCv.notify_all();
}

void RequestRecount()
{
	{
		std::lock_guard lock(gMutex);
		gRecount = true;
	}
	gCv.notify_all();
}

std::shared_ptr<const PlayerList> Get()
{
	std::lock_guard lock(gMutex);
	return gList;
}

uint64_t Version()
{
	return gVersion.load();
}

bool Has(const std::string& name)
{
	const auto list = Get();
	return std::any_of(list->players.begin(), list->players.end(), [&](const Player& p) { return p.name == name; });
}

std::string AvatarMessage(const std::string& name)
{
	json j = {{"msgType", 7}, {"name", name}, {"mime", ""}, {"data", ""}};
	const auto list = Get();
	for (const auto& p : list->players)
	{
		std::string bytes;
		if (p.name != name || p.avatar.empty() || !ReadFile(p.avatar, bytes, 4 * 1024 * 1024))
			continue;
		j["mime"] = AvatarMime(Profiles::PathU8(p.avatar.filename()));
		j["data"] = Base64(bytes);
		break;
	}
	return j.dump();
}
} // namespace adp::Players
