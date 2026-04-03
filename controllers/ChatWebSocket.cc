#include "ChatWebSocket.h"
#include <drogon/HttpRequest.h>
#include <drogon/orm/DbClient.h>
#include <json/json.h>
#include <string>

using namespace drogon;
using namespace drogon::orm;

std::unordered_map<int64_t, WebSocketConnectionPtr> ChatWebSocket::userConnections_;
std::mutex ChatWebSocket::connectionsMutex_;

int64_t ChatWebSocket::getUserId(const WebSocketConnectionPtr &conn) const {
    if (conn->hasContext()) {
        auto userIdPtr = conn->getContext<int64_t>();
        if (userIdPtr) return *userIdPtr;
    }
    return -1;
}

void ChatWebSocket::handleNewConnection(const HttpRequestPtr &req,
                                        const WebSocketConnectionPtr &wsConnPtr) {
    // 从 JwtFilter 获取用户ID（假设过滤器已存入 int64_t）
    auto userId = req->attributes()->get<int64_t>("user_id");

    wsConnPtr->setContext(std::make_shared<int64_t>(userId));

    // 处理重复登录：可关闭旧连接
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = userConnections_.find(userId);
        if (it != userConnections_.end() && it->second->connected()) {
            it->second->forceClose(); // 关闭旧连接
        }
        userConnections_[userId] = wsConnPtr;
    }

    LOG_INFO << "User " << userId << " connected. Online: " << userConnections_.size();

    // 上线后推送离线消息（需查询 offline_message 表）
    // pushOfflineMessages(userId, wsConnPtr); // 需实现
}

void ChatWebSocket::handleNewMessage(const WebSocketConnectionPtr &wsConnPtr,
                                     std::string &&message,
                                     const WebSocketMessageType &type) {
    if (type != WebSocketMessageType::Text) return; // 只处理文本JSON

    int64_t fromUserId = getUserId(wsConnPtr);
    if (fromUserId == -1) {
        wsConnPtr->forceClose();
        return;
    }

    Json::Value msgJson;
    Json::Reader reader;
    if (!reader.parse(message, msgJson)) {
        LOG_WARN << "Invalid JSON from user " << fromUserId;
        return;
    }

    // 检查必填字段
    if (!msgJson.isMember("chat_type") || !msgJson["chat_type"].isInt() ||
        !msgJson.isMember("target_id") || !msgJson["target_id"].isInt64() ||
        !msgJson.isMember("msg_type") || !msgJson["msg_type"].isInt()) {
        LOG_WARN << "Missing required fields";
        return;
    }

    int chatType = msgJson["chat_type"].asInt();   // 0-单聊 1-群聊
    int64_t targetId = msgJson["target_id"].asInt64();
    int msgType = msgJson["msg_type"].asInt();

    if (chatType == 0) {
        processSingleChat(wsConnPtr, msgJson, fromUserId);
    } else if (chatType == 1) {
        processGroupChat(wsConnPtr, msgJson, fromUserId);
    } else {
        LOG_WARN << "Invalid chat_type";
    }
}

bool ChatWebSocket::sendToUser(int64_t userId, const std::string &message) {
    WebSocketConnectionPtr conn;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = userConnections_.find(userId);
        if (it != userConnections_.end() && it->second->connected()) {
            conn = it->second;
        }
    }
    if (conn) {
        conn->send(message);
        return true;
    }
    return false;
}

void ChatWebSocket::processSingleChat(const WebSocketConnectionPtr &wsConnPtr,
                                      const Json::Value &msgJson,
                                      int64_t fromUserId) {
    int64_t toUserId = msgJson["target_id"].asInt64();

    // 检查是否是好友（可选，根据业务决定是否验证）
    // 此处省略好友验证

    // 查找对方是否在线
    WebSocketConnectionPtr targetConn;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = userConnections_.find(toUserId);
        if (it != userConnections_.end() && it->second->connected()) {
            targetConn = it->second;
        }
    }

    // 构造转发消息（可添加发送者信息）
    Json::Value forwardMsg = msgJson;
    forwardMsg["from_user_id"] = fromUserId;
    forwardMsg["timestamp"] = (Json::Int64)time(nullptr);

    std::string msgStr = forwardMsg.toStyledString();

    if (targetConn) {
        // 在线，直接转发
        targetConn->send(msgStr);
    } else {
        // 离线，存入 offline_message 表
        storeOfflineMessage(msgJson, fromUserId, toUserId, 0, toUserId);
    }
}

void ChatWebSocket::processGroupChat(const WebSocketConnectionPtr &wsConnPtr,
                                     const Json::Value &msgJson,
                                     int64_t fromUserId) {
    int64_t groupId = msgJson["target_id"].asInt64();

    // 查询群成员（需从数据库获取所有成员ID）
    auto db = app().getDbClient("serverDb");
    db->execSqlAsync(
        "SELECT user_id FROM group_member WHERE group_id = ? AND status = 1",
        [this, wsConnPtr, msgJson, fromUserId, groupId](const Result &r) {
            std::vector<int64_t> memberIds;
            for (auto &row : r) {
                memberIds.push_back(row["user_id"].as<int64_t>());
            }

            // 构造转发消息
            Json::Value forwardMsg = msgJson;
            forwardMsg["from_user_id"] = fromUserId;
            forwardMsg["timestamp"] = (Json::Int64)time(nullptr);
            std::string msgStr = forwardMsg.toStyledString();

            for (int64_t memberId : memberIds) {
                if (memberId == fromUserId) continue; // 不发送给自己

                // 查找在线连接
                WebSocketConnectionPtr targetConn;
                {
                    std::lock_guard<std::mutex> lock(connectionsMutex_);
                    auto it = userConnections_.find(memberId);
                    if (it != userConnections_.end() && it->second->connected()) {
                        targetConn = it->second;
                    }
                }

                if (targetConn) {
                    targetConn->send(msgStr);
                } else {
                    // 离线，存离线消息（每个成员一条）
                    storeOfflineMessage(msgJson, fromUserId, memberId, 1, groupId);
                }
            }
        },
        [fromUserId, groupId](const DrogonDbException &e) {
            LOG_ERROR << "Failed to get group members: " << e.base().what();
        },
        groupId
    );
}

void ChatWebSocket::storeOfflineMessage(const Json::Value &msgJson,
                                        int64_t fromUserId,
                                        int64_t toUserId,
                                        int chatType,
                                        int64_t targetId) {
    auto db = app().getDbClient("serverDb");

    // 从 msgJson 提取字段，对应 offline_message 表
    int msgType = msgJson["msg_type"].asInt();
    std::string content = msgJson.get("content", "").asString();
    std::string mediaUrl = msgJson.get("media_url", "").asString();
    std::string thumbUrl = msgJson.get("media_thumb_url", "").asString();
    int64_t mediaSize = msgJson.get("media_size", 0).asInt64();
    int mediaDuration = msgJson.get("media_duration", 0).asInt();
    std::string mediaFormat = msgJson.get("media_format", "").asString();

    // 设置过期时间（例如7天后）
    auto expireTime = trantor::Date::date().after(7 * 24 * 3600);

    db->execSqlAsync(
        "INSERT INTO offline_message (from_user_id, to_user_id, chat_type, target_id, msg_type, content, "
        "media_url, media_thumb_url, media_size, media_duration, media_format, expire_time) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12)",
        [](const Result &r) {
            LOG_DEBUG << "Offline message stored";
        },
        [](const DrogonDbException &e) {
            LOG_ERROR << "Failed to store offline message: " << e.base().what();
        },
        fromUserId, toUserId, chatType, targetId, msgType, content,
        mediaUrl, thumbUrl, mediaSize, mediaDuration, mediaFormat, expireTime
    );
}

void ChatWebSocket::handleConnectionClosed(const WebSocketConnectionPtr &wsConnPtr) {
    int64_t userId = getUserId(wsConnPtr);
    if (userId == -1) return;

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = userConnections_.find(userId);
        if (it != userConnections_.end() && it->second == wsConnPtr) {
            userConnections_.erase(it);
        }
    }

    LOG_INFO << "User " << userId << " disconnected. Online: " << userConnections_.size();
}