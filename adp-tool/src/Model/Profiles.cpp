#include <Model/Profiles.h>

#include <cctype>
#include <filesystem>
#include <fstream>

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

static fs::path PathFor(const std::string& dir, const std::string& id)
{
	return fs::path(dir) / (id + ".json");
}

bool Exists(const std::string& dir, const std::string& id)
{
	if (!ValidId(id))
		return false;
	std::error_code ec;
	return fs::exists(PathFor(dir, id), ec);
}

json List(const std::string& dir)
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

bool Read(const std::string& dir, const std::string& id, json& out)
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

bool Write(const std::string& dir, const std::string& id, const json& profile)
{
	if (!ValidId(id))
		return false;

	std::error_code ec;
	fs::create_directories(dir, ec); // no-op if it already exists

	// Write to a temp file, then rename over the target so a reader (possibly on
	// another machine sharing this folder) never sees a half-written profile.
	const fs::path finalPath = PathFor(dir, id);
	const fs::path tmpPath = fs::path(dir) / (id + ".json.tmp");
	{
		std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		out << profile.dump(2);
		if (!out)
			return false;
	}
	fs::rename(tmpPath, finalPath, ec);
	if (ec)
	{
		fs::remove(tmpPath, ec);
		return false;
	}
	return true;
}

bool Delete(const std::string& dir, const std::string& id)
{
	if (!ValidId(id))
		return false;
	std::error_code ec;
	fs::remove(PathFor(dir, id), ec);
	return !ec;
}
} // namespace adp::Profiles
