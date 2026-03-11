#include "ChatWebSocket.h"
#include <drogon/HttpRequest.h>
#include <vector>
#include <algorithm>

using namespace drogon;

static std::vector<WebSocketConnectionPtr> g_connections;

void ChatWebSocket::handleNewMessage(const WebSocketConnectionPtr &wsConnPtr,
                                     std::string &&message,
                                     const WebSocketMessageType &type) {
    for (auto &conn : g_connections) {
        if (conn != wsConnPtr && conn->connected()) {
            conn->send(message);
        }
    }
}

void ChatWebSocket::handleNewConnection(const HttpRequestPtr &req,
                                        const WebSocketConnectionPtr &wsConnPtr) {
    auto userId = req->getAttributes()->get<std::string>("user_id");
    // 存储为 shared_ptr<string>
    wsConnPtr->setContext(std::make_shared<std::string>(userId));

    g_connections.push_back(wsConnPtr);

    Json::Value welcome;
    welcome["type"] = "system";
    welcome["content"] = userId + " joined the chat";
    wsConnPtr->send(welcome.toStyledString());
}

void ChatWebSocket::handleConnectionClosed(const WebSocketConnectionPtr &wsConnPtr) {
    auto it = std::find(g_connections.begin(), g_connections.end(), wsConnPtr);
    if (it != g_connections.end()) {
        g_connections.erase(it);
    }

    if (wsConnPtr->hasContext()) {
        auto userIdPtr = wsConnPtr->getContext<std::string>();
        if (userIdPtr) {
            Json::Value leave;
            // 解引用 shared_ptr<string>
            leave["type"] = "system";
            leave["content"] = *userIdPtr + " left the chat";
            for (auto &conn : g_connections) {
                if (conn->connected()) {
                    conn->send(leave.toStyledString());
                }
            }
        }
    }
}