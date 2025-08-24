#include <Adp.h>

#define _HAS_STD_BYTE 0

#include <fstream>
#include <chrono>
#include <ctime>

#include <Model/Device.h>
#include <Model/Log.h>
#include <nfd.h>
#include <fmt/core.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

using namespace std;
typedef std::chrono::steady_clock Clock;

namespace adp {

static const char* TOOL_NAME = "ADP Tool";

class AdpApplication
{
public:
	AdpApplication();

	void MenuCallback() override;
	void RenderCallback() override;

	void LoadProfile();
	void SaveProfile();

private:
	string myLastProfile;
	Clock::time_point myLastUpdateTime;
};

void AdpApplication::LoadProfile()
{
	if (!Device::Pad()) {
		return;
	}

	nfdchar_t* rawOutPath = nullptr;
	const nfdchar_t* defaultPath = myLastProfile.empty() ? nullptr : myLastProfile.data();
	auto result = NFD_OpenDialog("json", nullptr, &rawOutPath);
	if (result != NFD_OKAY)
		return;
		
	string outPath(rawOutPath ? rawOutPath : "");
	free(rawOutPath);

	ifstream fileStream;
	fileStream.open(outPath);

	if (!fileStream.is_open())
	{
		Log::Writef("Could not read profile: %s", outPath.data());
		return;
	}

	try {
		myLastProfile = outPath;

		json j;

		fileStream >> j;
		fileStream.close();

		Device::LoadProfile(j, DGP_ALL);
	}
	catch (exception e) {
		Log::Writef("Could not read profile: %s", e.what());
	}
}

void AdpApplication::SaveProfile()
{
	if (!Device::Pad()) {
		return;
	}

	string path;
	nfdchar_t* rawOutPath = NULL;
	const nfdchar_t* defaultPath = myLastProfile.empty() ? nullptr : myLastProfile.data();
	auto result = NFD_SaveDialog("json", defaultPath, &rawOutPath);
	if (result != NFD_OKAY)
		return;

	string outPath(rawOutPath);
	if(outPath.find(".") == string::npos)
		outPath += ".json";

	free(rawOutPath);

	ofstream output_stream(outPath);
	if (!output_stream)
	{
		Log::Writef("Could not save profile: %s", outPath.data());
		return;
	}

	try {
		myLastProfile = outPath;

		json j;

		Device::SaveProfile(j, DGP_ALL);

		output_stream << j.dump(4);
		output_stream.close();
	}
	catch (exception e) {
		Log::Writef("Could not save profile: %s", e.what());
	}
}

void AdpApplication::RenderCallback()
{
	auto now = Clock::now();

	if (now - myLastUpdateTime > 10ms)
	{
		Device::Update();
		myLastUpdateTime = now;
	}
};

AdpApplication app;

void loop()
{
	app.Loop();
}

int Main(int argc, char** argv)
{
	Log::Init();

	auto versionString = fmt::format("{} {}.{}", TOOL_NAME, ADP_VERSION_MAJOR, ADP_VERSION_MINOR);
	auto now = std::time(0);
	std::string datetime = std::ctime(&now);
	Log::Writef("Application started: %s", versionString.data());
	Log::Writef("Starting at: %s", datetime.data());

	Device::Init();

	app.Run();

	Device::Shutdown();
	Log::Shutdown();

	return 0;
}

}