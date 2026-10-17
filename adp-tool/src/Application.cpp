#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <thread>
#include "Adp.h"
#include "Application.h"
#include "MSGQ.hpp"
#include "WebsocketServer.h"
#include "Model/Device.h"
#include "Model/Players.h"

namespace adp
{
Application::Application()
{
	OnInit();
}

Application::~Application()
{
	OnExit();
}

void Application::UpdateLoop(MSGQ<QueueMessage>& queue, WebsocketServer& websocketServer, std::atomic<bool>& running)
{
	// ~60Hz update loop, should be enough for the websocket UI.
	constexpr auto sleep_time = std::chrono::milliseconds(16);

	// The device list changes rarely, so rebroadcast it ~1Hz rather than every
	// tick. ponytail: periodic rebroadcast; move to on-change + request if it
	// shows up in traffic.
	constexpr int deviceListInterval = 60;
	int tickCount = 0;
	uint64_t playersVersion = 0;

	while (running.load())
	{
		// Update device data first (discovery + poll + publish snapshots).
		Tick();

		// Broadcast every pad's freshly published, immutable snapshot. Clients
		// pick the pad they show by its `pad` field.
		auto snapshots = Device::GetSnapshots();
		for (const auto& snapshot : snapshots ? *snapshots : std::vector<SensorSnapshot>{})
		{
			json sensorJson;
			Device::SnapshotToJson(snapshot, sensorJson);
			websocketServer.SendMessageToClients(sensorJson.dump());
		}

		// Rebroadcast the pad list so clients can enumerate pads. Runs regardless
		// of connection state so a client sees an empty list too.
		if (tickCount++ % deviceListInterval == 0)
		{
			json deviceListJson;
			Device::DeviceListToJson(deviceListJson);
			websocketServer.SendMessageToClients(deviceListJson.dump());
		}

		// Player list (msgType 3): resent after every completed background scan
		// (every 30s, or on request), so the latest one reaches new clients fast.
		if (const auto version = Players::Version(); version != playersVersion)
		{
			playersVersion = version;
			websocketServer.SendMessageToClients(Players::ToJson(*Players::Get()).dump());
		}

		// Per-pad selection (msgType 5): on change, or ~2s as a fallback for late joiners.
		constexpr int padStatesInterval = 120; // ~2s at ~60Hz
		if (Device::TakePadStatesDirty() || tickCount % padStatesInterval == 0)
		{
			json padStatesJson;
			Device::PadStatesToJson(padStatesJson);
			websocketServer.SendMessageToClients(padStatesJson.dump());
		}

		// Lights (msgType 4) are ~1KB per pad and change rarely, so broadcast them
		// only when a pad signals a change, or ~2s as a fallback so a client that
		// connected after the last change still gets the current lights.
		constexpr int lightsInterval = 120; // ~2s at ~60Hz
		if (snapshots && (Device::TakeLightsDirty() || tickCount % lightsInterval == 0))
		{
			for (const auto& snapshot : *snapshots)
			{
				json lightsJson;
				Device::LightsToJson(snapshot, lightsJson);
				websocketServer.SendMessageToClients(lightsJson.dump());
			}
		}

		// Drain and apply any inbound client messages. This runs on the
		// device-I/O thread, so device access stays single-threaded.
		while (auto item = queue.tryPop())
		{
			for (const auto& reply : Device::HandleClientMessage(item->data))
				websocketServer.SendMessageToClients(reply.dump());
		}

		std::this_thread::sleep_for(sleep_time);
	}

	std::cout << "Application update loop stopped" << std::endl;
}

void Application::OnInit()
{
	const auto startMsg = std::format("Application started: ADP Server v{}.{}", ADP_VERSION_MAJOR, ADP_VERSION_MINOR);
	std::cout << startMsg << std::endl;

	Device::Init();
}

void Application::OnExit()
{
	Device::Shutdown();
}

void Application::Tick()
{
	Device::Update();

	// Re-run discovery ~1Hz (not every 60Hz tick — hid_enumerate is not free) so
	// the device map keeps up with all attached pads and hot-plug. Discovery only
	// probes newly-seen paths and only auto-selects when nothing is connected, so
	// it does not disturb the streamed device.
	constexpr int discoverInterval = 60;
	static int discoverTick = 0;
	if (discoverTick++ % discoverInterval == 0)
	{
		Device::DiscoverNewDevices();
	}
}
} // namespace adp
