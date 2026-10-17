#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <map>
#include <string>
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
	// ~60Hz update loop, should be enough for the websocket UI. A fixed
	// cadence (sleep_until), so the time spent polling doesn't stretch a tick.
	constexpr auto tickInterval = std::chrono::microseconds(16667);
	auto nextTick = std::chrono::steady_clock::now();

	uint64_t playersVersion = 0;
	std::map<std::string, Sent> lastSnapshot; // per pad

	// Inbound messages taken off the queue but not handled yet.
	std::deque<Inbound> backlog;

	while (running.load())
	{
		nextTick += tickInterval;
		// Anything thrown here (a json dump of bad data, a filesystem error) is
		// logged, not fatal: the other pads and clients keep being served.
		try
		{
			RunTick(queue, websocketServer, playersVersion, lastSnapshot, backlog);
		}
		catch (const std::exception& e)
		{
			std::printf("UpdateLoop :: tick failed (%s)\n", e.what());
		}

		// After a stall (a slow probe), start over rather than racing to catch up.
		if (const auto now = std::chrono::steady_clock::now(); nextTick < now)
			nextTick = now;
		std::this_thread::sleep_until(nextTick);
	}

	std::cout << "Application update loop stopped" << std::endl;
}

void Application::RunTick(MSGQ<QueueMessage>& queue, WebsocketServer& websocketServer, uint64_t& playersVersion,
                          std::map<std::string, Sent>& lastSnapshot, std::deque<Inbound>& backlog)
{
	// An unchanged snapshot is skipped, but still resent at least this often:
	// clients treat a pad silent for 1s as gone.
	constexpr auto snapshotHeartbeat = std::chrono::milliseconds(250);

	// Update device data first (discovery + poll + publish snapshots).
	Tick();

	// A client connected or changed its subscription: resend every snapshot
	// now instead of at the next heartbeat.
	if (websocketServer.TakeSnapshotResend())
		lastSnapshot.clear();

	// Send every pad's freshly published, immutable snapshot to the clients
	// watching that pad.
	auto snapshots = Device::GetSnapshots();
	if (snapshots && websocketServer.HasClients())
	{
		const auto now = std::chrono::steady_clock::now();
		for (const auto& snapshot : *snapshots)
		{
			json sensorJson;
			Device::SnapshotToJson(snapshot, sensorJson);
			std::string message = sensorJson.dump();
			Sent& last = lastSnapshot[snapshot.path];
			if (message == last.json && now - last.at < snapshotHeartbeat)
				continue;
			websocketServer.SendSnapshot(snapshot.path, message);
			last = {std::move(message), now};
		}
		// Forget pads that are gone (a replug gets a fresh path).
		std::erase_if(lastSnapshot, [&](const auto& entry) {
			return std::none_of(snapshots->begin(), snapshots->end(),
			                    [&](const SensorSnapshot& s) { return s.path == entry.first; });
		});
	}

	// Everything below is sent on change only; Publish() replays the latest
	// copy to clients that connect later.

	// Pad list (msgType 2) and per-pad selection (msgType 5) change together:
	// pads coming/going, renames, and profile selections.
	if (Device::TakePadStatesDirty())
	{
		json deviceListJson, padStatesJson;
		Device::DeviceListToJson(deviceListJson);
		Device::PadStatesToJson(padStatesJson);
		websocketServer.Publish("pads", {deviceListJson.dump(), padStatesJson.dump()});
	}

	// Player list (msgType 3): after every completed background scan (every
	// 30s, or on request).
	if (const auto version = Players::Version(); version != playersVersion)
	{
		playersVersion = version;
		websocketServer.Publish("players", {Players::ToJson(*Players::Get()).dump()});
	}

	// Lights (msgType 4), one message per pad, ~1KB each.
	if (Device::TakeLightsDirty())
		websocketServer.Publish("lights", Device::LightsMessages());

	// Take every queued message into the backlog. A sensor/lights edit is
	// folded into the same client's previous edit of that pad if nothing
	// else for the pad came in between, so a slider drag that arrives faster
	// than the pad can take it collapses into its latest value instead of
	// building up lag.
	while (auto item = queue.tryPop())
	{
		json j = json::parse(item->data, nullptr, false);
		if (Device::IsConfigure(j))
		{
			const auto last = std::find_if(backlog.rbegin(), backlog.rend(), [&](const Inbound& b) {
				return b.message.is_object() && b.message.contains("pad") && b.message["pad"] == j["pad"];
			});
			if (last != backlog.rend() && last->sender == item->sender && Device::IsConfigure(last->message) &&
			    Device::MergeConfigure(last->message, j))
				continue;
		}
		if (backlog.size() >= MAX_BACKLOG)
		{
			std::printf("UpdateLoop :: backlog full, message from %s dropped\n", item->sender.c_str());
			for (const auto& reply : Device::Refuse(j, "the server is busy, try again"))
				websocketServer.SendTo(item->sender, reply.message.dump());
			continue;
		}
		backlog.push_back({std::move(item->sender), std::move(j)});
	}

	// Apply them. This runs on the device-I/O thread, so device access stays
	// single-threaded. Applying one can take several HID round-trips, so stop
	// after a time budget and leave the rest for the next tick: polling and
	// snapshots keep their cadence even when several clients drag sliders.
	const auto drainUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
	while (!backlog.empty())
	{
		const Inbound in = std::move(backlog.front());
		backlog.pop_front();
		for (const auto& reply : Device::HandleParsedMessage(in.message))
		{
			if (reply.toSender)
				websocketServer.SendTo(in.sender, reply.message.dump());
			else
				websocketServer.SendMessageToClients(reply.message.dump());
		}
		if (std::chrono::steady_clock::now() >= drainUntil)
			break;
	}
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
	// Run discovery ~1Hz (not every 60Hz tick — hid_enumerate is not free) so the
	// device map keeps up with hot-plug, including the very first tick. It only
	// probes newly-seen (or failed, after a backoff) paths, so connected pads
	// are not disturbed. Runs before Update() so a new pad is in this tick's
	// snapshots, which the lights broadcast it triggers relies on.
	constexpr unsigned discoverInterval = 60;
	static unsigned discoverTick = 0; // unsigned: wraps (after ~2 years) instead of overflowing
	if (discoverTick++ % discoverInterval == 0)
	{
		Device::DiscoverNewDevices();
	}

	Device::Update();
}
} // namespace adp
