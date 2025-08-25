#include "Adp.h"
#include <string>
#include <ixwebsocket/IXWebSocketServer.h>
#include "WebsocketServer.h"

namespace adp {
    WebsocketServer::WebsocketServer() = default;
    WebsocketServer::~WebsocketServer() = default;

    void WebsocketServer::Init() {
        // Run a server on localhost at a given port.
        // Bound host name, max connections and listen backlog can also be passed in as parameters.
        constexpr int port = 8008;
        const std::string host("127.0.0.1"); // If you need this server to be accessible on a different machine, use "0.0.0.0"
        ix::WebSocketServer server(port, host);

        server.setOnClientMessageCallback([](const std::shared_ptr<ix::ConnectionState>& connectionState, ix::WebSocket & webSocket, const ix::WebSocketMessagePtr & msg) {
            // The ConnectionState object contains information about the connection,
            // at this point only the client ip address and the port.
            std::printf("Remote ip: %s\n", connectionState->getRemoteIp().c_str());

            if (msg->type == ix::WebSocketMessageType::Open)
            {
                std::printf("New connection\n");

                // A connection state object is available, and has a default id
                // You can subclass ConnectionState and pass an alternate factory
                // to override it. It is useful if you want to store custom
                // attributes per connection (authenticated bool flag, attributes, etc...)
                std::printf("id: %s\n", connectionState->getId().c_str());

                // The uri the client did connect to.
                std::printf("Uri: %s\n", msg->openInfo.uri.c_str());

                std::printf("Headers:\n");
                for (auto it : msg->openInfo.headers)
                {
                    std::printf("\t%s: %s\n", it.first.c_str(), it.second.c_str());
                }
            }
            else if (msg->type == ix::WebSocketMessageType::Message)
            {
                // For an echo server, we just send back to the client whatever was received by the server
                // All connected clients are available in a std::set. See the broadcast cpp example.
                // Second parameter tells whether we are sending the message in binary or text mode.
                // Here we send it in the same mode as it was received.
                std::printf("Received: %s\n", msg->str.c_str());

                webSocket.send(msg->str, msg->binary);
            }
        });

        if (auto [success, error] = server.listen(); !success)
        {
            std::printf("ERROR: %s\n", error.c_str());
            return;
        }

        // Per message deflate connection is enabled by default. It can be disabled
        // which might be helpful when running on low power devices such as a Raspberry Pi
        server.disablePerMessageDeflate();

        // Run the server in the background. Server can be stoped by calling server.stop()
        server.start();

        std::printf("Websocket server started\n");

        // Block until server.stop() is called.
        server.wait();
    }
}
