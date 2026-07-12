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

	while (running.load())
	{
		// Update device data first (discovery + poll + publish snapshot).
		Tick();

		// Broadcast the freshly published, immutable snapshot to clients.
		auto snapshot = Device::GetSnapshot();
		if (snapshot && snapshot->connected)
		{
			json sensorJson;
			Device::SnapshotToJson(*snapshot, sensorJson);
			websocketServer.SendMessageToClients(sensorJson.dump());
		}

		// Rebroadcast the device list so clients can enumerate/switch pads. Runs
		// regardless of connection state so a client sees pads even when none is
		// selected yet.
		if (tickCount++ % deviceListInterval == 0)
		{
			json deviceListJson;
			Device::DeviceListToJson(deviceListJson);
			websocketServer.SendMessageToClients(deviceListJson.dump());
		}

		// Rebroadcast the stored-profile list so clients (across all venue
		// machines sharing the folder) can browse/load profiles. Sent on change
		// (a save/edit/delete flips a dirty flag) or ~2s as a fallback.
		// ponytail: polls the shared dir every ~2s; move to on-change/watch if the share is slow
		constexpr int profileListInterval = 120; // ~2s at ~60Hz
		if (Device::TakeProfilesDirty() || tickCount % profileListInterval == 0)
		{
			json profileListJson;
			Device::ProfileListToJson(profileListJson);
			websocketServer.SendMessageToClients(profileListJson.dump());
		}

		// Lights (msgType 4) are ~1KB and change rarely, so broadcast them only
		// when the pad signals a change/switch, or ~2s as a fallback so a client
		// that connected after the last change still gets the current lights.
		constexpr int lightsInterval = 120; // ~2s at ~60Hz
		if (snapshot && snapshot->connected &&
		    (Device::TakeLightsDirty() || tickCount % lightsInterval == 0))
		{
			json lightsJson;
			Device::LightsToJson(*snapshot, lightsJson);
			websocketServer.SendMessageToClients(lightsJson.dump());
		}

		// Drain and apply any inbound client messages. This runs on the
		// device-I/O thread, so device access stays single-threaded.
		while (auto item = queue.tryPop())
		{
			Device::HandleClientMessage(item->data);
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
