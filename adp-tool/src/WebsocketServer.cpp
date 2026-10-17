#include "Adp.h"
#include <cctype>
#include <cstdio>
#include <string>
#include <ixwebsocket/IXWebSocketServer.h>
#include "WebsocketServer.h"
#include "MSGQ.hpp"
#include "Model/Players.h"

namespace adp
{
// Above this much unsent data a client is considered stalled and gets no
// snapshots until it catches up (~2s of 60Hz snapshots for a few pads).
constexpr size_t MAX_BUFFERED_BYTES = 256 * 1024;

// Above this (a few avatars' worth) a client that isn't reading at all is
// disconnected rather than buffered for without bound; it gets everything
// again from the replay when it reconnects.
constexpr size_t MAX_STALLED_BYTES = 16 * 1024 * 1024;

// Sends `message` unless the client is stalled, which is then disconnected.
static void Deliver(ix::WebSocket& socket, const std::string& message)
{
	if (socket.bufferedAmount() > MAX_STALLED_BYTES)
	{
		socket.close(4008, "Too slow");
		return;
	}
	socket.send(message);
}

// %XX and '+' decoded, as browsers encode query values.
static std::string PercentDecode(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
		    std::isxdigit((unsigned char)s[i + 2]))
		{
			out.push_back((char)std::stoi(s.substr(i + 1, 2), nullptr, 16));
			i += 2;
		}
		else
			out.push_back(s[i] == '+' ? ' ' : s[i]);
	}
	return out;
}

// Compares in time independent of where the strings first differ.
static bool ConstantTimeEquals(const std::string& a, const std::string& b)
{
	if (a.size() != b.size())
		return false;
	unsigned char diff = 0;
	for (size_t i = 0; i < a.size(); ++i)
		diff |= (unsigned char)(a[i] ^ b[i]);
	return diff == 0;
}

bool AccessAllowed(const WebsocketAccess& access, const std::string& origin, const std::string& uri)
{
	if (!access.allowedOrigins.empty() && !access.allowedOrigins.contains(origin))
		return false;
	if (access.token.empty())
		return true;
	const auto q = uri.find('?');
	if (q == std::string::npos)
		return false;
	const std::string query = uri.substr(q + 1, uri.find('#', q) - q - 1);
	bool ok = false;
	for (size_t start = 0; start <= query.size();)
	{
		size_t end = query.find('&', start);
		if (end == std::string::npos)
			end = query.size();
		const std::string param = query.substr(start, end - start);
		if (param.rfind("token=", 0) == 0)
			ok |= ConstantTimeEquals(PercentDecode(param.substr(6)), access.token);
		start = end + 1;
	}
	return ok;
}

WebsocketServer::WebsocketServer() = default;
WebsocketServer::~WebsocketServer() = default;

void WebsocketServer::Init(MSGQ<QueueMessage>& queue, std::string host, WebsocketAccess access,
                           std::atomic<bool>& running)
{
	constexpr int port = 8008;

	auto srv = std::make_unique<ix::WebSocketServer>(port, host);

	srv->setOnClientMessageCallback([this, &queue, access](const std::shared_ptr<ix::ConnectionState>& connectionState,
	                                                       ix::WebSocket& webSocket,
	                                                       const ix::WebSocketMessagePtr& msg) {
		if (msg->type == ix::WebSocketMessageType::Open)
		{
			const auto originIt = msg->openInfo.headers.find("Origin");
			const std::string origin = originIt == msg->openInfo.headers.end() ? "" : originIt->second;
			if (!AccessAllowed(access, origin, msg->openInfo.uri))
			{
				std::printf("Refused connection from %s (origin '%s')\n", connectionState->getRemoteIp().c_str(),
				            origin.c_str());
				webSocket.close(4003, "Forbidden");
				return;
			}
			std::printf("New connection from %s (id: %s)\n", connectionState->getRemoteIp().c_str(),
			            connectionState->getId().c_str());

			webSocket.send(
			    nlohmann::json{{"msgType", 0}, {"protocol", PROTOCOL_VERSION}, {"server", ADP_VERSION_STR}}.dump());

			// Admit, then send the current device list, pad states, players and
			// lights so the client doesn't wait for the next change to see them.
			std::lock_guard<std::mutex> lock(replayMutex);
			{
				std::lock_guard<std::mutex> clientsLock(clientsMutex);
				clients[&webSocket] = {connectionState->getId()};
			}
			for (const auto& [key, messages] : replay)
				for (const auto& m : messages)
					webSocket.send(m);
			snapshotResend = true;
		}
		else if (msg->type == ix::WebSocketMessageType::Close)
		{
			std::printf("Client %s WebSocket disconnected, reason: %s\n", connectionState->getId().c_str(),
			            msg->closeInfo.reason.c_str());
			std::lock_guard<std::mutex> lock(clientsMutex);
			clients.erase(&webSocket);
		}
		else if (msg->type == ix::WebSocketMessageType::Message)
		{
			{
				std::lock_guard<std::mutex> lock(clientsMutex);
				if (!clients.contains(&webSocket))
					return; // refused, still closing
			}
			// Avatars are file-only and can be MBs, so answer them here, to the
			// asking client only, instead of on the device thread. The name is
			// looked up in the scanned player list, never used as a path.
			// Subscriptions are per connection, so they are handled here too.
			if (msg->str.find("\"getAvatar\"") != std::string::npos ||
			    msg->str.find("\"subscribe\"") != std::string::npos)
			{
				const auto j = nlohmann::json::parse(msg->str, nullptr, false);
				if (j.is_object() && j.contains("getAvatar") && j["getAvatar"].is_string())
				{
					Deliver(webSocket, Players::AvatarMessage(j["getAvatar"].get<std::string>()));
					return;
				}
				if (j.is_object() && j.contains("subscribe") && j["subscribe"].is_string())
				{
					{
						std::lock_guard<std::mutex> lock(clientsMutex);
						clients[&webSocket].subscription = j["subscribe"].get<std::string>();
					}
					snapshotResend = true;
					return;
				}
			}
			// Enqueue the message by value; the device thread drains and applies it.
			queue.push(QueueMessage(msg->str, connectionState->getId()));
		}
	});

	if (auto [success, error] = srv->listen(); !success)
	{
		// Exit instead of running headless, so a supervisor (systemd) restarts us.
		std::printf("ERROR: cannot listen on %s:%d: %s\n", host.c_str(), port, error.c_str());
		failed = true;
		running = false;
		return;
	}

	// Per message deflate connection is enabled by default. It can be disabled
	// which might be helpful when running on low power devices such as a Raspberry Pi
	srv->disablePerMessageDeflate();

	// Run the server in the background. Server can be stopped by calling Stop().
	srv->start();
	std::printf("Websocket server started on ws://%s:%d%s\n", host.c_str(), port,
	            access.token.empty() ? "" : " (token required)");

	// Publish the owned server so other threads can broadcast, then block
	// until Stop(). Our own condition (not ix's wait(), which misses a stop
	// that came first) also covers a Stop() issued before we got here.
	std::unique_lock<std::mutex> lock(serverMutex);
	server = std::move(srv);
	stopCv.wait(lock, [this] { return stopRequested; });
	ix::WebSocketServer* s = server.get();
	lock.unlock();

	// Outside the lock: stop() joins the connection threads.
	s->stop();
}

void WebsocketServer::SendMessageToClients(const std::string& message)
{
	std::lock_guard<std::mutex> lock(serverMutex);
	if (server == nullptr)
		return;

	for (const auto& socket : server->getClients())
	{
		{
			std::lock_guard<std::mutex> clientsLock(clientsMutex);
			if (!clients.contains(socket.get()))
				continue;
		}
		Deliver(*socket, message);
	}
}

void WebsocketServer::SendTo(const std::string& clientId, const std::string& message)
{
	std::lock_guard<std::mutex> lock(serverMutex);
	if (server == nullptr)
		return;

	std::shared_ptr<ix::WebSocket> target;
	{
		std::lock_guard<std::mutex> clientsLock(clientsMutex);
		for (const auto& socket : server->getClients())
		{
			auto it = clients.find(socket.get());
			if (it != clients.end() && it->second.id == clientId)
			{
				target = socket;
				break;
			}
		}
	}
	// Outside clientsMutex: a failing send closes the socket, and ix runs our
	// Close handler (which takes clientsMutex) synchronously on this thread.
	if (target)
		Deliver(*target, message);
}

bool WebsocketServer::TakeSnapshotResend()
{
	return snapshotResend.exchange(false);
}

bool WebsocketServer::HasClients()
{
	std::lock_guard<std::mutex> lock(clientsMutex);
	return !clients.empty();
}

void WebsocketServer::Publish(const std::string& key, std::vector<std::string> messages)
{
	std::lock_guard<std::mutex> lock(replayMutex);
	for (const auto& m : messages)
		SendMessageToClients(m);
	replay[key] = std::move(messages);
}

void WebsocketServer::SendSnapshot(const std::string& pad, const std::string& message)
{
	std::lock_guard<std::mutex> lock(serverMutex);
	if (server == nullptr)
		return;

	for (const auto& socket : server->getClients())
	{
		{
			std::lock_guard<std::mutex> clientsLock(clientsMutex);
			auto it = clients.find(socket.get());
			if (it == clients.end() || (it->second.subscription != "*" && it->second.subscription != pad))
				continue;
		}
		if (socket->bufferedAmount() > MAX_BUFFERED_BYTES)
			continue;
		socket->send(message);
	}
}

void WebsocketServer::Stop()
{
	{
		std::lock_guard<std::mutex> lock(serverMutex);
		stopRequested = true;
	}
	stopCv.notify_all();
}
} // namespace adp
