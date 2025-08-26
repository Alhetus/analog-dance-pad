#ifndef WEBSOCKETSERVER_H
#define WEBSOCKETSERVER_H

#include "MSGQ.hpp"
#include "ixwebsocket/IXWebSocketServer.h"

namespace adp {
    class WebsocketServer {
    public:
        WebsocketServer();
        ~WebsocketServer();

        void Init(MSGQ<QueueMessage*> &queue);
        void SendMessageToClients(const std::string& message) const;
    private:
        ix::WebSocketServer* wsServer = nullptr;
    };
}

#endif //WEBSOCKETSERVER_H
