#ifndef WEBSOCKETSERVER_H
#define WEBSOCKETSERVER_H

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "MSGQ.hpp"
#include "ixwebsocket/IXWebSocketServer.h"

namespace adp
{
class WebsocketServer
{
  public:
	WebsocketServer();
	~WebsocketServer();

	// Runs the server on `host` (e.g. "0.0.0.0" to accept phones/tablets on
	// the LAN); blocks until Stop() is called. If the port can't be opened it
	// clears `running` (so the whole process exits) and sets Failed().
	void Init(MSGQ<QueueMessage>& queue, std::string host, std::atomic<bool>& running);

	// Broadcasts a message to all connected clients. Thread-safe.
	void SendMessageToClients(const std::string& message);

	// Sends a message to one client, if it is still connected. Thread-safe.
	void SendTo(const ix::WebSocket* client, const std::string& message);

	// True if any client is connected. Thread-safe.
	bool HasClients();

	// Broadcasts `messages` and remembers them under `key` (replacing what was
	// there), so a client that connects later gets the latest copy on open
	// instead of waiting for the next change. Thread-safe.
	void Publish(const std::string& key, std::vector<std::string> messages);

	// Sends one pad's live snapshot to the clients subscribed to that pad
	// ({subscribe: "*" | <pad path> | ""}, default "*"). Snapshots are
	// droppable, so a client whose send buffer is backed up (asleep phone with
	// the socket still open) is skipped instead of buffering without bound.
	void SendSnapshot(const std::string& pad, const std::string& message);

	// Requests shutdown, unblocking Init(). Thread-safe, idempotent, and safe
	// to call before Init() has published the server.
	void Stop();

	bool Failed() const { return failed.load(); }

  private:
	// Owns the ix server for its whole lifetime (no dangling stack pointer).
	// Guarded because it is published on the server thread and read from the
	// device thread.
	std::mutex serverMutex;
	std::condition_variable stopCv;
	bool stopRequested = false;
	std::unique_ptr<ix::WebSocketServer> server;
	std::atomic<bool> failed{false};

	// Latest Publish()ed messages, replayed to each new client. Held across
	// the broadcast so a new client never receives an older copy after a newer one.
	std::mutex replayMutex;
	std::map<std::string, std::vector<std::string>> replay;

	// Pad each client watches; a client missing here watches all pads.
	std::mutex subMutex;
	std::map<const ix::WebSocket*, std::string> subscriptions;
};
} // namespace adp

#endif // WEBSOCKETSERVER_H
