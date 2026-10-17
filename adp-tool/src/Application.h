#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "MSGQ.hpp"
#include "WebsocketServer.h"
#include "Model/Device.h"

namespace adp
{
class Application
{
  public:
	Application();
	~Application();

	// The single device-I/O loop: discovers devices, polls sensors, publishes
	// the snapshot, broadcasts it, and applies inbound client commands. Runs
	// until `running` is cleared (graceful shutdown).
	void UpdateLoop(MSGQ<QueueMessage>& queue, WebsocketServer& websocketServer, std::atomic<bool>& running);

	// Most inbound messages waiting to be handled; past it new ones are refused.
	static constexpr size_t MAX_BACKLOG = 256;

	// Sensor/lights edits are applied to one pad at most this often; the ones
	// arriving meanwhile fold into the waiting one. Each edit is HID round-trips,
	// and the firmware blanks the LEDs on every sensor write, so a slider drag
	// at full client rate would flicker them.
	static constexpr auto CONFIGURE_INTERVAL = std::chrono::milliseconds(50);

	using Clock = std::chrono::steady_clock;

	// Who is owed an answer to a message: its sender and its requestId (null if none).
	struct Asker
	{
		std::string sender;
		nlohmann::json requestId;
		bool operator==(const Asker&) const = default;
	};

	// A parsed inbound message (discarded if it wasn't JSON) and its sender.
	struct Inbound
	{
		std::string sender;
		nlohmann::json message;
		std::vector<Asker> folded; // earlier configures folded into this one: answered too
	};

	// Index of the first backlog message that may be applied at `now`, or
	// npos. A configure waits until CONFIGURE_INTERVAL after its pad's last
	// one (`lastConfigure`), a pad in `busy` (saving) waits altogether, and
	// nothing else for a waiting pad overtakes its first message.
	static size_t NextDue(const std::deque<Inbound>& backlog,
	                      const std::map<std::string, Clock::time_point>& lastConfigure,
	                      const std::set<std::string>& busy, Clock::time_point now);

	// Appends `in` to the backlog. A configure folds into the pad's waiting one
	// if that is the pad's last message (whichever client sent it: last writer
	// wins per field), so slider drags that arrive faster than the pad takes
	// them collapse instead of building up lag. False if the backlog is full.
	static bool Enqueue(std::deque<Inbound>& backlog, Inbound in);

  private:
	// The last snapshot sent for one pad, also as the struct, so an unchanged
	// pad is skipped without serializing it.
	struct Sent
	{
		std::string json;
		Clock::time_point at;
		SensorSnapshot snapshot;
	};

	// One tick of UpdateLoop.
	void RunTick(MSGQ<QueueMessage>& queue, WebsocketServer& websocketServer);

	uint64_t playersVersion = 0;
	std::map<std::string, Sent> lastSnapshot;               // per pad
	std::deque<Inbound> backlog;                            // taken off the queue, not handled yet
	std::map<std::string, Clock::time_point> lastConfigure; // per pad
	unsigned discoverTick = 0;                              // unsigned: wraps (after ~2 years) instead of overflowing
};
} // namespace adp
