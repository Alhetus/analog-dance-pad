#include "Adp.h"
#include <cstdio>
#include <string>
#include <ixwebsocket/IXWebSocketServer.h>
#include "WebsocketServer.h"
#include "MSGQ.hpp"

namespace adp
{
WebsocketServer::WebsocketServer() = default;
WebsocketServer::~WebsocketServer() = default;

void WebsocketServer::Init(MSGQ<QueueMessage>& queue)
{
	// Run a server on localhost at a given port.
	// Bound host name, max connections and listen backlog can also be passed in as parameters.
	constexpr int port = 8008;
	const std::string host(
	    "127.0.0.1"); // If you need this server to be accessible on a different machine, use "0.0.0.0"

	auto srv = std::make_unique<ix::WebSocketServer>(port, host);

	srv->setOnClientMessageCallback([&queue](const std::shared_ptr<ix::ConnectionState>& connectionState,
	                                         ix::WebSocket& webSocket, const ix::WebSocketMessagePtr& msg) {
		(void)webSocket;
		// The ConnectionState object contains information about the connection,
		// at this point only the client ip address and the port.

		if (msg->type == ix::WebSocketMessageType::Open)
		{
			std::printf("New connection from %s (id: %s), uri: %s\n", connectionState->getRemoteIp().c_str(),
			            connectionState->getId().c_str(), msg->openInfo.uri.c_str());
		}
		else if (msg->type == ix::WebSocketMessageType::Close)
		{
			std::printf("Client %s WebSocket disconnected, reason: %s\n", connectionState->getId().c_str(),
			            msg->closeInfo.reason.c_str());
		}
		else if (msg->type == ix::WebSocketMessageType::Message)
		{
			// Enqueue the message by value; the device thread drains and applies it.
			queue.push(QueueMessage(msg->str));
		}
	});

	if (auto [success, error] = srv->listen(); !success)
	{
		std::printf("ERROR: %s\n", error.c_str());
		return;
	}

	// Per message deflate connection is enabled by default. It can be disabled
	// which might be helpful when running on low power devices such as a Raspberry Pi
	srv->disablePerMessageDeflate();

	// Run the server in the background. Server can be stopped by calling Stop().
	srv->start();
	std::printf("Websocket server started on ws://%s:%d\n", host.c_str(), port);

	// Publish the owned server so other threads can broadcast/stop.
	{
		std::lock_guard<std::mutex> lock(serverMutex);
		server = std::move(srv);
	}

	// Block until Stop() is called.
	server->wait();
}

void WebsocketServer::SendMessageToClients(const std::string& message)
{
	std::lock_guard<std::mutex> lock(serverMutex);
	if (server == nullptr)
	{
		return;
	}

	for (const auto& socket : server->getClients())
	{
		socket->send(message);
	}
}

void WebsocketServer::Stop()
{
	std::lock_guard<std::mutex> lock(serverMutex);
	if (server != nullptr)
	{
		server->stop();
	}
}
} // namespace adp
