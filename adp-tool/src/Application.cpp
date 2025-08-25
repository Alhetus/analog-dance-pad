#include <thread>
#include "Adp.h"
#include "Application.h"

#include <iostream>

#include "Model/Device.h"

namespace adp {
    Application::Application() {
        OnInit();
    }

    Application::~Application() {
        OnExit();
    }

    void Application::UpdateLoop() {
        // 100Hz update loop, should be enough for the websocket UI
        constexpr auto sleep_time = std::chrono::milliseconds(10);

        while (true)
        {
            std::this_thread::sleep_for(sleep_time);
            Tick();
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
