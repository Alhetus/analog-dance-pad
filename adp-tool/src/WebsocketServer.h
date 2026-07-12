#ifndef WEBSOCKETSERVER_H
#define WEBSOCKETSERVER_H

#include <memory>
#include <mutex>
#include <string>

#include "MSGQ.hpp"
#include "ixwebsocket/IXWebSocketServer.h"

namespace adp
{
class WebsocketServer
{
  public:
	WebsocketServer();
	~WebsocketServer();

	// Runs the server; blocks until Stop() is called.
	void Init(MSGQ<QueueMessage>& queue);

	// Broadcasts a message to all connected clients. Thread-safe.
	void SendMessageToClients(const std::string& message);

	// Stops the server, unblocking Init(). Thread-safe and idempotent.
	void Stop();

  private:
	// Owns the ix server for its whole lifetime (no dangling stack pointer).
	// Guarded because it is published on the server thread and read/stopped
	// from the device thread and the signal path.
	std::mutex serverMutex;
	std::unique_ptr<ix::WebSocketServer> server;
};
} // namespace adp

#endif // WEBSOCKETSERVER_H
