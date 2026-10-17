#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN // no winsock.h: ixwebsocket includes winsock2.h
#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>
#endif

#ifdef _MSC_VER
// C4702 (unreachable code) inside argparse's templates, reported at
// code generation where /external can't silence it.
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
#include <argparse/argparse.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <ixwebsocket/IXNetSystem.h>
#include "Adp.h"
#include "Application.h"
#include "MSGQ.hpp"
#include "WebsocketServer.h"
#include <Model/Device.h>
#include <Model/Players.h>
#include <Model/Profiles.h>

namespace
{
// Cleared by the signal handler to request a graceful shutdown.
std::atomic<bool> g_running{true};
// Set once shutdown (device save/close) has finished.
std::atomic<bool> g_shutdownDone{false};
} // namespace

static void HandleSignal(int)
{
	g_running.store(false);
}

#ifdef _WIN32
// Closing the console window, logoff and shutdown don't raise SIGINT/SIGTERM;
// Windows kills the process as soon as this handler returns (or after ~5s),
// so hold it until main() has saved and closed the pads.
static BOOL WINAPI HandleConsoleCtrl(DWORD type)
{
	g_running.store(false);
	if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT)
		while (!g_shutdownDone.load())
			Sleep(20);
	return TRUE;
}
#endif

int main(int argc, char** argv)
{
	// Line-buffer the log: under systemd/journald stdout is a pipe, which is
	// otherwise fully buffered and shows log lines late (or never, on a kill).
#ifdef _WIN32
	// MSVC has no line buffering (_IOLBF means full) and aborts on size 0.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	// Names and paths are logged as UTF-8.
	SetConsoleOutputCP(CP_UTF8);
	// Sleep() otherwise rounds up to the 15.6ms system tick on some machines:
	// the 60Hz loop would run at ~32Hz and every back-to-back HID feature
	// report would wait a whole tick instead of its 2ms gap.
	timeBeginPeriod(1);
	// argv is in the ANSI codepage; the paths below are read as UTF-8.
	std::vector<std::string> utf8Args;
	{
		int n = 0;
		if (wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &n))
		{
			for (int i = 0; i < n; ++i)
			{
				const int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
				std::string s(len > 0 ? len - 1 : 0, '\0');
				WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len, nullptr, nullptr);
				utf8Args.push_back(std::move(s));
			}
			LocalFree(wargv);
		}
	}
	std::vector<char*> utf8Argv;
	for (auto& a : utf8Args)
		utf8Argv.push_back(a.data());
	if (!utf8Argv.empty())
	{
		argc = (int)utf8Argv.size();
		argv = utf8Argv.data();
	}
#else
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif

	argparse::ArgumentParser args("adp-tool",
	                              std::to_string(ADP_VERSION_MAJOR) + "." + std::to_string(ADP_VERSION_MINOR));
	// No default_value(): with one, argparse accepts a bare "--host" and keeps
	// the default. Defaults are applied below instead.
	args.add_argument("--profiles-dir").nargs(1).help("root of this server's local profile store (default: profiles)");
	args.add_argument("--local-profiles-dir")
	    .nargs(1)
	    .help("ITGmania's LocalProfiles folder (usually on a network share), the source of the player list");
	args.add_argument("--host").nargs(1).help(
	    "interface to listen on; 0.0.0.0 lets phones/tablets in (default: 127.0.0.1)");
	args.add_argument("--allowed-origin")
	    .append()
	    .nargs(1)
	    .help("Origin allowed to connect, e.g. http://cab1:5173 (repeatable; default: any)");
	args.add_argument("--token").nargs(1).help("if set, clients must connect with ?token=<token>");
	try
	{
		args.parse_args(argc, argv);
	}
	catch (const std::exception& e)
	{
		std::cerr << e.what() << "\n" << args;
		return 2;
	}

	const auto opt = [&](const char* name, const char* fallback) {
		return args.present<std::string>(name).value_or(fallback);
	};
	const std::string profilesDir = opt("--profiles-dir", "profiles");
	if (profilesDir.empty())
	{
		std::cerr << "--profiles-dir must not be empty\n";
		return 2;
	}
	adp::WebsocketAccess access;
	for (const auto& o :
	     args.present<std::vector<std::string>>("--allowed-origin").value_or(std::vector<std::string>{}))
		access.allowedOrigins.insert(o);
	access.token = opt("--token", "");
	// Open by design (README, "Security model"): the venue LAN and its users are trusted.
	if (access.allowedOrigins.empty() && access.token.empty())
		std::printf("Any client may connect (no --allowed-origin / --token)\n");

	// Must be set before Application (which constructs Device) is created below.
	adp::Device::SetProfilesDir(adp::Profiles::U8Path(profilesDir));
	adp::Players::Start(adp::Profiles::U8Path(opt("--local-profiles-dir", "")), adp::Profiles::U8Path(profilesDir),
	                    std::chrono::seconds(30));

	// Required on Windows
	ix::initNetSystem();

	// Graceful shutdown on Ctrl-C / termination.
	std::signal(SIGINT, HandleSignal);
	std::signal(SIGTERM, HandleSignal);
#ifdef _WIN32
	SetConsoleCtrlHandler(HandleConsoleCtrl, TRUE);
#endif

	MSGQ<QueueMessage> queue;
	bool failed = false;

	{
		adp::Application application;
		adp::WebsocketServer websocketServer;

		// Start the single device-I/O application thread.
		auto applicationThread = std::thread(&adp::Application::UpdateLoop, &application, std::ref(queue),
		                                     std::ref(websocketServer), std::ref(g_running));

		// Start the websocket server thread.
		std::thread websocketServerThread(&adp::WebsocketServer::Init, &websocketServer, std::ref(queue),
		                                  opt("--host", "127.0.0.1"), access, std::ref(g_running), 8008);

		// The application loop exits when g_running is cleared by the signal handler
		// (or by the WebSocket server when it can't listen).
		applicationThread.join();

		// Stop the server (unblocks Init), then join.
		std::cout << "Shutting down..." << std::endl;
		websocketServer.Stop();
		websocketServerThread.join();

		failed = websocketServer.Failed();
		// application's destructor runs here, saving/closing the pads (Device::Shutdown).
	}
	// The pads are saved: a Windows logoff/shutdown may go ahead now.
	g_shutdownDone = true;

	// Last: joining the scanner can block for a long time on a dead network
	// share, and the pads must be saved before a supervisor gives up on us.
	adp::Players::Stop();

	// Required on Windows
	ix::uninitNetSystem();
#ifdef _WIN32
	timeEndPeriod(1);
#endif
	return failed ? 1 : 0;
}
