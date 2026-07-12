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

namespace adp {
    Application::Application() {
        OnInit();
    }

    Application::~Application() {
        OnExit();
    }

    void Application::UpdateLoop(MSGQ<QueueMessage>& queue, WebsocketServer& websocketServer, std::atomic<bool>& running) {
        // ~60Hz update loop, should be enough for the websocket UI.
        constexpr auto sleep_time = std::chrono::milliseconds(16);

        while (running.load())
        {
            // Update device data first (discovery + poll + publish snapshot).
            Tick();

            // Broadcast the freshly published, immutable snapshot to clients.
            if (auto snapshot = Device::GetSnapshot(); snapshot && snapshot->connected) {
                json sensorJson;
                Device::SnapshotToJson(*snapshot, sensorJson);
                websocketServer.SendMessageToClients(sensorJson.dump());
            }

            // Drain and apply any inbound client messages. This runs on the
            // device-I/O thread, so device access stays single-threaded.
            while (auto item = queue.tryPop()) {
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

        // Try to connect to new devices if the expected count is not connected
        constexpr int expectedDeviceCount = 1; // TODO: This should be a configurable value
        auto numberOfConnectedDevices = Device::DeviceNumber();

        if (numberOfConnectedDevices < expectedDeviceCount)
        {
            Device::DiscoverNewDevices();
        }
    }
}
