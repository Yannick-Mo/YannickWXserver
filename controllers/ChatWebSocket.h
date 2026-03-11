#pragma once
#include <drogon/WebSocketController.h>

class ChatWebSocket : public drogon::WebSocketController<ChatWebSocket> {
public:
    virtual void handleNewMessage(const drogon::WebSocketConnectionPtr &wsConnPtr,
                                  std::string &&message,
                                  const drogon::WebSocketMessageType &type) override;

    virtual void handleNewConnection(const drogon::HttpRequestPtr &req,
                                     const drogon::WebSocketConnectionPtr &wsConnPtr) override;

    virtual void handleConnectionClosed(const drogon::WebSocketConnectionPtr &wsConnPtr) override;

    WS_PATH_LIST_BEGIN
        // WS_PATH_ADD 的第二个参数是 HTTP 方法，第三个及以后是过滤器名称
        WS_PATH_ADD("/chat", drogon::Get, "JwtFilter");
    WS_PATH_LIST_END
};