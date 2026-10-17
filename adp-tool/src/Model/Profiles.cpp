#include <Model/Profiles.h>
#include <Model/Reporter.h> // slot counts

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace adp::Profiles
{
bool ValidId(const std::string& id)
{
	if (id.empty() || id.size() > 128)
		return false;
	for (char c : id)
	{
		const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
		if (!ok)
			return false;
	}
	return true;
}

std::string SlugifyId(const std::string& name)
{
	std::string out;
	bool lastDash = false;
	for (char c : name)
	{
		char lc = (char)std::tolower((unsigned char)c);
		if ((lc >= 'a' && lc <= 'z') || (lc >= '0' && lc <= '9'))
		{
			out.push_back(lc);
			lastDash = false;
		}
		else if (!lastDash && !out.empty())
		{
			out.push_back('-');
			lastDash = true;
		}
	}
	while (!out.empty() && out.back() == '-')
		out.pop_back();
	if (out.empty())
		out = "profile";
	if (out.size() > 128)
		out.resize(128);
	return out;
}

Path U8Path(const std::string& utf8)
{
	return Path(std::u8string(utf8.begin(), utf8.end()));
}

std::string PathU8(const Path& p)
{
	const std::u8string s = p.u8string();
	return std::string(s.begin(), s.end());
}

std::string EscapeName(const std::string& name)
{
	static const char* reserved[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
	                                 "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
	                                 "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
	// Windows treats "CON.txt" as CON too, so compare the part before any dot.
	std::string stem = name.substr(0, name.find('.'));
	for (auto& c : stem)
		c = (char)std::toupper((unsigned char)c);
	bool isReserved = false;
	for (const char* r : reserved)
		isReserved = isReserved || stem == r;

	std::string out;
	for (size_t i = 0; i < name.size(); ++i)
	{
		const unsigned char c = (unsigned char)name[i];
		const bool edge = i == 0 || i + 1 == name.size();
		const bool bad = c < 0x20 || c == 0x7f || std::strchr("<>:\"/\\|?*%", c) != nullptr || (c == '.' && edge) ||
		                 (c == ' ' && i + 1 == name.size()) || (i == 0 && isReserved);
		if (bad)
		{
			char buf[4];
			std::snprintf(buf, sizeof(buf), "%%%02X", c);
			out += buf;
		}
		else
			out.push_back((char)c);
	}
	return out;
}

std::string UnescapeName(const std::string& escaped)
{
	std::string out;
	for (size_t i = 0; i < escaped.size(); ++i)
	{
		if (escaped[i] == '%' && i + 2 < escaped.size() && std::isxdigit((unsigned char)escaped[i + 1]) &&
		    std::isxdigit((unsigned char)escaped[i + 2]))
		{
			out.push_back((char)std::stoi(escaped.substr(i + 1, 2), nullptr, 16));
			i += 2;
		}
		else
			out.push_back(escaped[i]);
	}
	return out;
}

std::string ValidUtf8(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size();)
	{
		const unsigned char c = (unsigned char)s[i];
		// Sequence length and the allowed range of its second byte (which rules
		// out overlongs, surrogates and code points past U+10FFFF).
		size_t n = 0;
		unsigned char lo = 0x80, hi = 0xBF;
		if (c < 0x80)
			n = 1;
		else if (c >= 0xC2 && c <= 0xDF)
			n = 2;
		else if (c >= 0xE0 && c <= 0xEF)
		{
			n = 3;
			lo = c == 0xE0 ? 0xA0 : 0x80;
			hi = c == 0xED ? 0x9F : 0xBF;
		}
		else if (c >= 0xF0 && c <= 0xF4)
		{
			n = 4;
			lo = c == 0xF0 ? 0x90 : 0x80;
			hi = c == 0xF4 ? 0x8F : 0xBF;
		}
		bool ok = n > 0 && i + n <= s.size();
		for (size_t k = 1; ok && k < n; ++k)
		{
			const unsigned char b = (unsigned char)s[i + k];
			ok = k == 1 ? (b >= lo && b <= hi) : (b >= 0x80 && b <= 0xBF);
		}
		if (ok)
		{
			out.append(s, i, n);
			i += n;
		}
		else
		{
			out += "\xEF\xBF\xBD";
			++i;
		}
	}
	return out;
}

Path PlayerPadDir(const Path& profilesDir, const std::string& player, const std::string& pad)
{
	return profilesDir / "players" / U8Path(EscapeName(player)) / U8Path(EscapeName(pad));
}

static fs::path PathFor(const Path& dir, const std::string& id)
{
	return dir / (id + ".json");
}

bool Exists(const Path& dir, const std::string& id)
{
	if (!ValidId(id))
		return false;
	std::error_code ec;
	return fs::exists(PathFor(dir, id), ec);
}

json List(const Path& dir)
{
	json arr = json::array();
	std::error_code ec;
	if (!fs::is_directory(dir, ec))
		return arr;

	for (const auto& entry : fs::directory_iterator(dir, ec))
	{
		if (ec)
			break;
		const auto& p = entry.path();
		if (p.extension() != ".json")
			continue; // skips the ".json.tmp" write-in-progress files too
		if (!ValidId(PathU8(p.stem())))
			continue; // e.g. a hand-copied "My Profile.json": its id could never be sent back

		std::ifstream in(p, std::ios::binary);
		if (!in)
			continue;
		try
		{
			json profile = json::parse(in);
			if (!profile.is_object())
				continue;
			profile["id"] = PathU8(p.stem());
			arr.push_back(std::move(profile));
		}
		catch (const std::exception&)
		{
			// A half-written or corrupt file should not break the whole list.
			continue;
		}
	}
	// Directory order differs between platforms and filesystems.
	const auto savedAt = [](const json& p) {
		return p.contains("savedAt") && p["savedAt"].is_number() ? p["savedAt"].get<double>() : 0.0;
	};
	std::sort(arr.begin(), arr.end(), [&](const json& a, const json& b) {
		return std::pair(savedAt(a), a["id"].get<std::string>()) < std::pair(savedAt(b), b["id"].get<std::string>());
	});
	return arr;
}

bool Read(const Path& dir, const std::string& id, json& out)
{
	if (!ValidId(id))
		return false;
	std::ifstream in(PathFor(dir, id), std::ios::binary);
	if (!in)
		return false;
	try
	{
		out = json::parse(in);
		return true;
	}
	catch (const std::exception&)
	{
		return false;
	}
}

bool ReadProfile(const Path& dir, const std::string& id, json& out)
{
	if (!Read(dir, id, out) || !out.is_object())
		return false;
	out["id"] = id;
	return true;
}

std::mutex& StoreMutex()
{
	static std::mutex m;
	return m;
}

// Writes `data` to `p` and flushes it to the disk, not just the OS cache.
static bool WriteDurably(const fs::path& p, const std::string& data)
{
#ifdef _WIN32
	FILE* f = nullptr;
	if (_wfopen_s(&f, p.c_str(), L"wb") != 0)
		f = nullptr;
#else
	FILE* f = std::fopen(p.c_str(), "wb");
#endif
	if (!f)
		return false;
	bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size() && std::fflush(f) == 0;
#ifdef _WIN32
	ok = ok && _commit(_fileno(f)) == 0;
#else
	ok = ok && fsync(fileno(f)) == 0;
#endif
	return std::fclose(f) == 0 && ok;
}

bool Write(const Path& dir, const std::string& id, const json& profile)
{
	if (!ValidId(id))
		return false;

	std::error_code ec;
	fs::create_directories(dir, ec); // no-op if it already exists

	// Write to a synced temp file, then rename over the target, so neither a
	// reader nor a power cut ever leaves a half-written or empty profile.
	const fs::path finalPath = PathFor(dir, id);
	const fs::path tmpPath = dir / (id + ".json.tmp");
	if (!WriteDurably(tmpPath, profile.dump(2)))
	{
		fs::remove(tmpPath, ec);
		return false;
	}
	fs::rename(tmpPath, finalPath, ec);
	if (ec)
	{
		fs::remove(tmpPath, ec);
		return false;
	}
#ifndef _WIN32
	// Persist the rename itself (the directory entry).
	if (int fd = ::open(dir.c_str(), O_RDONLY); fd >= 0)
	{
		fsync(fd);
		::close(fd);
	}
#endif
	return true;
}

// "#rrggbb" in lowercase, as a live pad's lights serialize, from "#RRGGBB" or
// "rrggbb"; black for anything else (what the pad itself would make of it).
static std::string NormalizeColor(const json& v)
{
	std::string hex = v.is_string() ? v.get<std::string>() : "";
	if (!hex.empty() && hex[0] == '#')
		hex.erase(0, 1);
	if (hex.size() != 6 || !std::all_of(hex.begin(), hex.end(), [](char c) { return std::isxdigit((unsigned char)c); }))
		return "#000000";
	for (auto& c : hex)
		c = (char)std::tolower((unsigned char)c);
	return "#" + hex;
}

// Copies `key` from `in` to `out` if it is a number, clamped to [lo, hi]
// (integers rounded).
static void CopyNumber(const json& in, json& out, const char* key, double lo, double hi, bool integer)
{
	if (!in.contains(key) || !in[key].is_number())
		return;
	const double v = std::clamp(in[key].get<double>(), lo, hi);
	if (integer)
		out[key] = (int)std::lround(v);
	else
		out[key] = v;
}

// One lights slot rebuilt from its known fields; null stays null (a disabled slot).
static json SanitizeLightRule(const json& in)
{
	if (!in.is_object())
		return nullptr;
	json out = json::object();
	for (const char* key : {"fadeOn", "fadeOff"})
		out[key] = in.contains(key) && in[key].is_boolean() && in[key].get<bool>();
	for (const char* key : {"onColor", "offColor", "onFadeColor", "offFadeColor"})
		out[key] = NormalizeColor(in.contains(key) ? in[key] : json());
	return out;
}

static json SanitizeLedMapping(const json& in)
{
	if (!in.is_object())
		return nullptr;
	json out = json::object();
	for (const char* key : {"lightRuleIndex", "sensorIndex", "ledIndexBegin", "ledIndexEnd"})
	{
		out[key] = 0;
		CopyNumber(in, out, key, 0, 255, true);
	}
	return out;
}

json Sanitize(const json& profile)
{
	if (!profile.is_object() || !profile.contains("sensorCount") || !profile["sensorCount"].is_number_integer() ||
	    !profile.contains("sensors") || !profile["sensors"].is_array())
		return nullptr;

	// One entry per sensor (a short array would load only partly), and no
	// more slots than a pad has, so nothing unbounded reaches the disk.
	const int64_t sensorCount = profile["sensorCount"].get<int64_t>();
	if (sensorCount < 0 || sensorCount > SENSOR_COUNT_MAX || (int64_t)profile["sensors"].size() != sensorCount)
		return nullptr;

	json out = json::object();
	out["sensorCount"] = sensorCount;
	out["sensors"] = json::array();
	for (const auto& in : profile["sensors"])
	{
		json sensor = json::object();
		if (in.is_object())
		{
			CopyNumber(in, sensor, "threshold", 0, 1, false);
			CopyNumber(in, sensor, "releaseThreshold", 0, 1, false);
			CopyNumber(in, sensor, "resistorValue", 0, 255, true);
		}
		out["sensors"].push_back(std::move(sensor));
	}

	const struct
	{
		const char* key;
		int max;
		json (*slot)(const json&);
	} lights[] = {{"lightRules", MAX_LIGHT_RULES, SanitizeLightRule},
	              {"ledMappings", MAX_LED_MAPPINGS, SanitizeLedMapping}};
	for (const auto& [key, max, slot] : lights)
	{
		if (!profile.contains(key) || !profile[key].is_array())
			continue;
		if ((int)profile[key].size() > max)
			return nullptr;
		out[key] = json::array();
		for (const auto& in : profile[key])
			out[key].push_back(slot(in));
	}

	const bool hasDescription = profile.contains("description") && profile["description"].is_string();
	out["description"] = hasDescription ? profile["description"] : json("");
	if (out["description"].get_ref<const std::string&>().size() > MAX_DESCRIPTION_BYTES)
		return nullptr;
	return out;
}

bool Delete(const Path& dir, const std::string& id)
{
	if (!ValidId(id))
		return false;
	std::error_code ec;
	fs::remove(PathFor(dir, id), ec);
	return !ec;
}
} // namespace adp::Profiles
