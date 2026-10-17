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

  private:
	// The last snapshot sent for one pad.
	struct Sent
	{
		std::string json;
		std::chrono::steady_clock::time_point at;
	};

	// A parsed inbound message (discarded if it wasn't JSON) and its sender.
	struct Inbound
	{
		std::string sender;
		nlohmann::json message;
	};

	// One tick of UpdateLoop.
	static void RunTick(MSGQ<QueueMessage>& queue, WebsocketServer& websocketServer, uint64_t& playersVersion,
	                    std::map<std::string, Sent>& lastSnapshot, std::deque<Inbound>& backlog);

	static void OnInit();
	static void OnExit();
	static void Tick();
};
} // namespace adp
