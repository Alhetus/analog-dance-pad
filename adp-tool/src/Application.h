#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
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

	// A parsed inbound message (discarded if it wasn't JSON) and its sender.
	struct Inbound
	{
		std::string sender;
		nlohmann::json message;
	};

	// Index of the first backlog message that may be applied at `now`, or
	// npos. A configure waits until CONFIGURE_INTERVAL after its pad's last
	// one (`lastConfigure`), and nothing else for that pad overtakes it.
	static size_t NextDue(const std::deque<Inbound>& backlog,
	                      const std::map<std::string, Clock::time_point>& lastConfigure, Clock::time_point now);

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
	static void RunTick(MSGQ<QueueMessage>& queue, WebsocketServer& websocketServer, uint64_t& playersVersion,
	                    std::map<std::string, Sent>& lastSnapshot, std::deque<Inbound>& backlog,
	                    std::map<std::string, Clock::time_point>& lastConfigure);

	static void OnInit();
	static void OnExit();
	static void Tick();
};
} // namespace adp
