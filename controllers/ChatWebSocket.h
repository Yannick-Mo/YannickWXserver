#pragma once
#include <drogon/WebSocketController.h>
#include <unordered_map>
#include <array>
#include <shared_mutex>
#include <atomic>

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
    static constexpr size_t SHARD_COUNT = 64;

    struct Shard {
        std::unordered_map<int64_t, drogon::WebSocketConnectionPtr> connections;
        std::shared_mutex mtx;
    };
    static std::array<Shard, SHARD_COUNT> shards_;
    static std::atomic<int64_t> onlineCount_;

    static Shard& getShard(int64_t userId);

    int64_t getUserId(const drogon::WebSocketConnectionPtr &conn) const;
    void closeOldConnection(const drogon::WebSocketConnectionPtr &oldConn);
    void storeOfflineMessage(const Json::Value &msgJson,
                             int64_t fromUserId,
                             int64_t toUserId,
                             int chatType,
                             int64_t targetId,
                             bool isBackup = false);
    void processSingleChat(const drogon::WebSocketConnectionPtr &wsConnPtr,
                           const Json::Value &msgJson,
                           int64_t fromUserId,
                           int64_t clientTempId);
    void processGroupChat(const drogon::WebSocketConnectionPtr &wsConnPtr,
                          const Json::Value &msgJson,
                          int64_t fromUserId,
                          int64_t clientTempId);
    void sendAck(int64_t toUserId, int64_t clientTempId, int status);

    void pushOfflineMessages(int64_t userId, const drogon::WebSocketConnectionPtr &wsConnPtr);

};