#pragma once
#include <atomic>
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

  private:
	static void OnInit();
	static void OnExit();
	static void Tick();
};
} // namespace adp
