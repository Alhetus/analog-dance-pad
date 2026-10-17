#ifndef WEBSOCKETSERVER_H
#define WEBSOCKETSERVER_H

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "MSGQ.hpp"
#include "ixwebsocket/IXWebSocketServer.h"

namespace adp
{
// Sent to every client on connect ({msgType 0, protocol}). Bumped on any
// incompatible message change, so an old client cached on a phone can tell it
// is out of date instead of half working.
constexpr int PROTOCOL_VERSION = 1;

// Who may connect. Both are optional and off by default: the server is open
// to any client by design (README, "Security model"). Browsers don't apply
// the same-origin policy to WebSockets, so these are the knobs if a venue
// ever needs one.
struct WebsocketAccess
{
	std::set<std::string> allowedOrigins; // empty: any Origin
	std::string token;                    // non-empty: required as ?token=<token>
};

// True if a connection with this Origin header and request URI may connect.
// The token is ?token=<t> in the query string, percent-decoded.
bool AccessAllowed(const WebsocketAccess& access, const std::string& origin, const std::string& uri);

class WebsocketServer
{
  public:
	WebsocketServer();
	~WebsocketServer();

	// Runs the server on `host` (e.g. "0.0.0.0" to accept phones/tablets on
	// the LAN); blocks until Stop() is called. If the port can't be opened it
	// clears `running` (so the whole process exits) and sets Failed().
	void Init(MSGQ<QueueMessage>& queue, std::string host, WebsocketAccess access, std::atomic<bool>& running);

	// Broadcasts a message to all connected clients. Thread-safe.
	void SendMessageToClients(const std::string& message);

	// Sends a message to one client (by connection id), if it is still
	// connected. Thread-safe.
	void SendTo(const std::string& clientId, const std::string& message);

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

	// True (and cleared) if a client connected or changed its subscription
	// since the last call, so snapshots are resent now rather than at the next
	// heartbeat. Thread-safe.
	bool TakeSnapshotResend();

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
	std::atomic<bool> snapshotResend{false};

	// Latest Publish()ed messages, replayed to each new client. Held across
	// the broadcast so a new client never receives an older copy after a newer one.
	std::mutex replayMutex;
	std::map<std::string, std::vector<std::string>> replay;

	// Admitted clients, added on open and removed on close. Only these get
	// messages, so a refused connection receives nothing while it closes.
	// Sockets are only ever reached through server->getClients() (which keeps
	// them alive); the pointer is just the lookup key.
	// Lock order: replayMutex -> serverMutex -> clientsMutex.
	struct Client
	{
		std::string id;
		std::string subscription = "*"; // pad path, "*" for all, "" for none
	};
	std::mutex clientsMutex;
	std::map<const ix::WebSocket*, Client> clients;
};
} // namespace adp

#endif // WEBSOCKETSERVER_H
