#pragma once
#include "MSGQ.hpp"
#include "WebsocketServer.h"

namespace adp {
    class Application {
    public:
        Application();
        ~Application();

        [[noreturn]] void UpdateLoop(MSGQ<QueueMessage*> &queue, const WebsocketServer &websocketServer);
    private:
        static void OnInit();
        static void OnExit();
        static void Tick();
    };
}
