#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include "Application.h"
#include "MSGQ.hpp"
#include "WebsocketServer.h"
#include <Model/Device.h>
#include <Model/Players.h>

using json = nlohmann::json;

namespace
{
// Cleared by the signal handler to request a graceful shutdown.
std::atomic<bool> g_running{true};
} // namespace

static void HandleSignal(int)
{
	g_running.store(false);
}

int main(int argc, char** argv)
{
	// Line-buffer the log: under systemd/journald stdout is a pipe, which is
	// otherwise fully buffered and shows log lines late (or never, on a kill).
#ifdef _WIN32
	// MSVC has no line buffering (_IOLBF means full) and aborts on size 0.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
#else
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif

	// --profiles-dir <path>: root of this server's local profile store.
	// --local-profiles-dir <path>: ITGmania's LocalProfiles folder (usually on
	//   a network share), the source of the player list.
	// --host <addr>: interface to listen on; "0.0.0.0" lets phones/tablets in.
	// Must be set before Application (which constructs Device) is created below.
	std::string profilesDir = "profiles", localProfilesDir, host = "127.0.0.1";
	for (int i = 1; i + 1 < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--profiles-dir")
			profilesDir = argv[i + 1];
		else if (arg == "--local-profiles-dir")
			localProfilesDir = argv[i + 1];
		else if (arg == "--host")
			host = argv[i + 1];
	}
	adp::Device::SetProfilesDir(profilesDir);
	adp::Players::Start(localProfilesDir, profilesDir.empty() ? "profiles" : profilesDir, std::chrono::seconds(30));

	// Required on Windows
	ix::initNetSystem();

	// Graceful shutdown on Ctrl-C / termination.
	std::signal(SIGINT, HandleSignal);
	std::signal(SIGTERM, HandleSignal);

	MSGQ<QueueMessage> queue;

	adp::Application application;
	adp::WebsocketServer websocketServer;

	// Start the single device-I/O application thread.
	auto applicationThread = std::thread(&adp::Application::UpdateLoop, &application, std::ref(queue),
	                                     std::ref(websocketServer), std::ref(g_running));

	// Start the websocket server thread.
	std::thread websocketServerThread(&adp::WebsocketServer::Init, &websocketServer, std::ref(queue), host,
	                                  std::ref(g_running));

	// The application loop exits when g_running is cleared by the signal handler
	// (or by the WebSocket server when it can't listen).
	applicationThread.join();

	// Tear down the websocket side: stop the server (unblocks Init) and wake any
	// queue waiters, then join.
	std::cout << "Shutting down..." << std::endl;
	websocketServer.Stop();
	queue.stop();
	websocketServerThread.join();
	adp::Players::Stop();

	// application's destructor runs here, saving/closing the device (Device::Shutdown).

	// Required on Windows
	ix::uninitNetSystem();
	return websocketServer.Failed() ? 1 : 0;
}
