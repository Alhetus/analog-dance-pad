#include <Model/Profiles.h>

#include <cctype>
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

json Sanitize(const json& profile)
{
	if (!profile.is_object() || !profile.contains("sensorCount") || !profile["sensorCount"].is_number_integer() ||
	    !profile.contains("sensors") || !profile["sensors"].is_array())
		return nullptr;

	json out = json::object();
	out["sensorCount"] = profile["sensorCount"];
	out["sensors"] = profile["sensors"];
	if (profile.contains("releaseThreshold") && profile["releaseThreshold"].is_number())
		out["releaseThreshold"] = profile["releaseThreshold"];
	for (const char* key : {"lightRules", "ledMappings"})
		if (profile.contains(key) && profile[key].is_array())
			out[key] = profile[key];
	out["description"] =
	    profile.contains("description") && profile["description"].is_string() ? profile["description"] : json("");
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
