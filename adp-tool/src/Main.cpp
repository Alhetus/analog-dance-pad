#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include "Application.h"
#include "MSGQ.hpp"
#include "WebsocketServer.h"

using json = nlohmann::json;

int main()
{
    // Required on Windows
    ix::initNetSystem();

    MSGQ<QueueMessage*> queue;

    adp::Application application;
    adp::WebsocketServer websocketServer;

    // Start the ADP application thread
    auto applicationThread = std::thread(&adp::Application::UpdateLoop, &application, std::ref(queue), std::ref(websocketServer));

    // Start the websocket server thread
    std::thread websocketServerThread(&adp::WebsocketServer::Init, &websocketServer, std::ref(queue));

    // Wait for the thread to finish execution (will never happen atm)
    applicationThread.join();
    websocketServerThread.join();

    // Required on Windows
    ix::uninitNetSystem();
    return 0;
}
