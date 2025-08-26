#include <thread>
#include <iostream>
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

    void Application::UpdateLoop(MSGQ<QueueMessage*> &queue, const WebsocketServer &websocketServer) {
        // ~60Hz update loop, should be enough for the websocket UI
        constexpr auto sleep_time = std::chrono::milliseconds(16);

        while (true)
        {
            std::this_thread::sleep_for(sleep_time);
            Tick(); // Update data first before handling messages

            // Send sensor data to clients if device is connected
            if (Device::Pad() != nullptr) {
                json sensorJson;

                Device::GetAllSensorStatesAsJson(sensorJson);
                std::string sensorJsonString = sensorJson.dump();
                websocketServer.SendMessageToClients(sensorJsonString);
            }

            QueueMessage *item = nullptr;

            // Try to get new messages from the queue on each iteration
            while ((item = queue.popElem()) != nullptr) {
                std::cout << "Got message with data : " << item->data << std::endl;
                delete item;
            }
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
