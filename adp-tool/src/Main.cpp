#include <atomic>
#include <csignal>
#include <iostream>
#include <thread>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include "Application.h"
#include "MSGQ.hpp"
#include "WebsocketServer.h"

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

int main()
{
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
	std::thread websocketServerThread(&adp::WebsocketServer::Init, &websocketServer, std::ref(queue));

	// The application loop exits when g_running is cleared by the signal handler.
	applicationThread.join();

	// Tear down the websocket side: stop the server (unblocks Init's wait()) and
	// wake any queue waiters, then join.
	std::cout << "Shutting down..." << std::endl;
	websocketServer.Stop();
	queue.stop();
	websocketServerThread.join();

	// application's destructor runs here, saving/closing the device (Device::Shutdown).

	// Required on Windows
	ix::uninitNetSystem();
	return 0;
}
