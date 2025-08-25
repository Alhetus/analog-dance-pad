#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include "Application.h"
#include "WebsocketServer.h"

using json = nlohmann::json;

int main()
{
    // Required on Windows
    ix::initNetSystem();

    adp::Application application;
    adp::WebsocketServer websocketServer;

    // Start the ADP application thread
    std::thread applicationThread(&adp::Application::UpdateLoop, &application);

    // Start the websocket server thread
    std::thread websocketServerThread(&adp::WebsocketServer::Init, &websocketServer);

    // Wait for the thread to finish execution (will never happen atm)
    applicationThread.join();
    websocketServerThread.join();

    // Required on Windows
    ix::uninitNetSystem();
    return 0;
}
