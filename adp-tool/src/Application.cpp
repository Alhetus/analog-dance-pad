#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include "Adp.h"
#include "Application.h"
#include "MSGQ.hpp"
#include "WebsocketServer.h"
#include "Model/Device.h"
#include "Model/Players.h"
#include "Model/Profiles.h"

namespace adp
{
Application::Application()
{
	std::cout << std::format("Application started: ADP Server v{}.{}", ADP_VERSION_MAJOR, ADP_VERSION_MINOR)
	          << std::endl;
	Device::Init();
}

Application::~Application()
{
	Device::Shutdown();
}

void Application::UpdateLoop(MSGQ<QueueMessage>& queue, WebsocketServer& websocketServer, std::atomic<bool>& running)
{
	// ~60Hz update loop, should be enough for the websocket UI. A fixed
	// cadence (sleep_until), so the time spent polling doesn't stretch a tick.
	constexpr auto tickInterval = std::chrono::microseconds(16667);
	auto nextTick = std::chrono::steady_clock::now();

	while (running.load())
	{
		nextTick += tickInterval;
		// Anything thrown here (a json dump of bad data, a filesystem error) is
		// logged, not fatal: the other pads and clients keep being served.
		try
		{
			RunTick(queue, websocketServer);
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

// The pad a message is for, or "".
static std::string PadOf(const nlohmann::json& m)
{
	return m.is_object() && m.contains("pad") && m["pad"].is_string() ? m["pad"].get<std::string>() : "";
}

static nlohmann::json RequestIdOf(const nlohmann::json& m)
{
	return m.is_object() && m.contains("requestId") ? m["requestId"] : nlohmann::json();
}

size_t Application::NextDue(const std::deque<Inbound>& backlog,
                            const std::map<std::string, Clock::time_point>& lastConfigure,
                            const std::set<std::string>& busy, Clock::time_point now)
{
	std::set<std::string> held; // pads with an earlier message still waiting
	for (size_t i = 0; i < backlog.size(); ++i)
	{
		const std::string pad = PadOf(backlog[i].message);
		if (pad.empty())
			return i;
		if (held.contains(pad))
			continue;
		const auto last = lastConfigure.find(pad);
		if (busy.contains(pad) || (Device::IsConfigure(backlog[i].message) && last != lastConfigure.end() &&
		                           now - last->second < CONFIGURE_INTERVAL))
		{
			held.insert(pad);
			continue;
		}
		return i;
	}
	return std::string::npos;
}

bool Application::Enqueue(std::deque<Inbound>& backlog, Inbound in)
{
	if (Device::IsConfigure(in.message))
	{
		const std::string pad = PadOf(in.message);
		const auto last =
		    std::find_if(backlog.rbegin(), backlog.rend(), [&](const Inbound& b) { return PadOf(b.message) == pad; });
		if (last != backlog.rend() && Device::IsConfigure(last->message))
		{
			// The folded message is answered under the later one's sender and
			// requestId; the earlier asker gets a copy (see RunTick).
			const Asker earlier{last->sender, RequestIdOf(last->message)};
			json merged = last->message;
			merged.erase("requestId");
			if (Device::MergeConfigure(merged, in.message))
			{
				last->message = std::move(merged);
				const Asker later{in.sender, RequestIdOf(in.message)};
				if (earlier != later &&
				    std::find(last->folded.begin(), last->folded.end(), earlier) == last->folded.end())
					last->folded.push_back(earlier);
				std::erase(last->folded, later);
				last->sender = std::move(in.sender);
				return true;
			}
		}
	}
	if (backlog.size() >= MAX_BACKLOG)
		return false;
	backlog.push_back(std::move(in));
	return true;
}

void Application::RunTick(MSGQ<QueueMessage>& queue, WebsocketServer& websocketServer)
{
	// An unchanged snapshot is skipped, but still resent at least this often:
	// clients treat a pad silent for 2s as gone.
	constexpr auto snapshotHeartbeat = std::chrono::milliseconds(250);

	// Run discovery ~1Hz (not every 60Hz tick — hid_enumerate is not free) so the
	// device map keeps up with hot-plug, including the very first tick. It only
	// probes newly-seen (or failed, after a backoff) paths, so connected pads
	// are not disturbed; their stage 2 runs in the background and Update()
	// collects the finished ones.
	constexpr unsigned discoverInterval = 60;
	if (discoverTick++ % discoverInterval == 0)
		Device::DiscoverNewDevices();

	// Update device data (collect, poll, publish snapshots).
	Device::Update();

	// A client connected or changed its subscription: resend every snapshot
	// now instead of at the next heartbeat.
	if (websocketServer.TakeSnapshotResend())
		lastSnapshot.clear();

	// The pad list goes before the snapshots below, so a client never gets a
	// snapshot of a pad it doesn't know yet. Everything published here is sent
	// on change only; Publish() replays the latest copy to clients that
	// connect later.

	// Pad list (msgType 2) and per-pad selection (msgType 5) change together:
	// pads coming/going, renames, and profile selections.
	if (Device::TakePadStatesDirty())
	{
		json deviceListJson, padStatesJson;
		Device::DeviceListToJson(deviceListJson);
		Device::PadStatesToJson(padStatesJson);
		websocketServer.Publish("pads", {Profiles::Dump(deviceListJson), Profiles::Dump(padStatesJson)});
	}

	// Send every pad's freshly published, immutable snapshot to the clients
	// watching that pad.
	auto snapshots = Device::GetSnapshots();
	if (snapshots && websocketServer.HasClients())
	{
		const auto now = std::chrono::steady_clock::now();
		for (const auto& snapshot : *snapshots)
		{
			Sent& last = lastSnapshot[snapshot.path];
			const bool heartbeatDue = now - last.at >= snapshotHeartbeat;
			if (!heartbeatDue && snapshot == last.snapshot)
				continue;
			json sensorJson;
			Device::SnapshotToJson(snapshot, sensorJson);
			std::string message = Profiles::Dump(sensorJson);
			// Raw averages differ more often than their quantized JSON does.
			if (!heartbeatDue && message == last.json)
			{
				last.snapshot = snapshot;
				continue;
			}
			websocketServer.SendSnapshot(snapshot.path, message);
			last = {std::move(message), now, snapshot};
		}
		// Forget pads that are gone (a replug gets a fresh path).
		std::erase_if(lastSnapshot, [&](const auto& entry) {
			return std::none_of(snapshots->begin(), snapshots->end(),
			                    [&](const SensorSnapshot& s) { return s.path == entry.first; });
		});
	}

	// Player list (msgType 3): after every completed background scan (every
	// 30s, or on request).
	if (const auto version = Players::Version(); version != playersVersion)
	{
		playersVersion = version;
		websocketServer.Publish("players", {Profiles::Dump(Players::ToJson(*Players::Get()))});
	}

	// Lights (msgType 4), one message per pad, ~1KB each.
	if (Device::TakeLightsDirty())
		websocketServer.Publish("lights", Device::LightsMessages());

	// Take every queued message into the backlog.
	while (auto item = queue.tryPop())
	{
		json j = json::parse(item->data, nullptr, false);
		const std::string sender = item->sender;
		if (!Enqueue(backlog, {std::move(item->sender), j, {}}))
		{
			std::printf("UpdateLoop :: backlog full, message from %s dropped\n", sender.c_str());
			for (const auto& reply : Device::Refuse(j, "the server is busy, try again"))
				websocketServer.SendTo(sender, Profiles::Dump(reply.message));
		}
	}

	// Apply them. This runs on the device-I/O thread, so device access stays
	// single-threaded. Applying one can take several HID round-trips, so stop
	// after a time budget and leave the rest for the next tick: polling and
	// snapshots keep their cadence even when several clients drag sliders.
	// ponytail: a message is never split, so a profile load with lights (up to
	// ~45 reports, ~150ms) still overruns the budget; queue its HID writes per
	// pad and drain them across ticks if that ever shows.
	const std::set<std::string> busy = Device::BusyPads();
	const auto drainUntil = Clock::now() + std::chrono::milliseconds(8);
	for (size_t i; (i = NextDue(backlog, lastConfigure, busy, Clock::now())) != std::string::npos;)
	{
		const Inbound in = std::move(backlog[i]);
		backlog.erase(backlog.begin() + (std::ptrdiff_t)i);
		if (Device::IsConfigure(in.message))
			lastConfigure[PadOf(in.message)] = Clock::now();
		for (const auto& reply : Device::HandleParsedMessage(in.message))
		{
			if (!reply.toSender)
			{
				websocketServer.SendMessageToClients(Profiles::Dump(reply.message));
				continue;
			}
			websocketServer.SendTo(in.sender, Profiles::Dump(reply.message));
			for (const Asker& asker : in.folded)
			{
				json copy = reply.message;
				if (asker.requestId.is_null())
					copy.erase("requestId");
				else
					copy["requestId"] = asker.requestId;
				websocketServer.SendTo(asker.sender, Profiles::Dump(copy));
			}
		}
		if (Clock::now() >= drainUntil)
			break;
	}
	std::erase_if(lastConfigure,
	              [](const auto& entry) { return Clock::now() - entry.second > std::chrono::seconds(1); });
}
} // namespace adp
