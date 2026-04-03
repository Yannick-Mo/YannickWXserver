#pragma once
#include <drogon/WebSocketController.h>
#include <unordered_map>
#include <mutex>

class ChatWebSocket : public drogon::WebSocketController<ChatWebSocket> {
public:
    virtual void handleNewMessage(const drogon::WebSocketConnectionPtr &wsConnPtr,
                                  std::string &&message,
                                  const drogon::WebSocketMessageType &type) override;

    virtual void handleNewConnection(const drogon::HttpRequestPtr &req,
                                     const drogon::WebSocketConnectionPtr &wsConnPtr) override;

    virtual void handleConnectionClosed(const drogon::WebSocketConnectionPtr &wsConnPtr) override;

    static bool sendToUser(int64_t userId, const std::string &message);


    WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/chat", drogon::Get, "JwtFilter");
    WS_PATH_LIST_END

private:
    // 用户ID -> 连接映射
    static std::unordered_map<int64_t, drogon::WebSocketConnectionPtr> userConnections_;
    static std::mutex connectionsMutex_;

    // 辅助函数
    int64_t getUserId(const drogon::WebSocketConnectionPtr &conn) const;
    void storeOfflineMessage(const Json::Value &msgJson, int64_t fromUserId, int64_t toUserId, int chatType, int64_t targetId);
    void processSingleChat(const drogon::WebSocketConnectionPtr &wsConnPtr, const Json::Value &msgJson, int64_t fromUserId);
    void processGroupChat(const drogon::WebSocketConnectionPtr &wsConnPtr, const Json::Value &msgJson, int64_t fromUserId);
};