#include "ChatWebSocket.h"
#include "utils/AsyncMessageWriter.h"
#include <drogon/HttpRequest.h>
#include <drogon/orm/DbClient.h>
#include <json/json.h>
#include <string>
#include <shared_mutex>

using namespace drogon;
using namespace drogon::orm;

std::array<ChatWebSocket::Shard, ChatWebSocket::SHARD_COUNT> ChatWebSocket::shards_;
std::atomic<int64_t> ChatWebSocket::onlineCount_{0};

static std::string fastJson(const Json::Value& v) {
    thread_local Json::FastWriter w;
    return w.write(v);
}

static std::string buildAck(int64_t clientTempId, int status) {
    thread_local std::string s;
    s.clear();
    s += "{\"type\":\"msg_ack\",\"client_temp_id\":";
    s += std::to_string(clientTempId);
    s += ",\"status\":";
    s += std::to_string(status);
    s += "}";
    return s;
}

ChatWebSocket::Shard& ChatWebSocket::getShard(int64_t userId) {
    return shards_[static_cast<size_t>(userId) % SHARD_COUNT];
}

int64_t ChatWebSocket::getUserId(const WebSocketConnectionPtr &conn) const {
    if (conn && conn->hasContext()) {
        auto userIdPtr = conn->getContext<int64_t>();
        if (userIdPtr) return *userIdPtr;
    }
    return -1;
}

void ChatWebSocket::closeOldConnection(const WebSocketConnectionPtr &oldConn) {
    if (oldConn && oldConn->connected()) {
        oldConn->forceClose();
    }
}

void ChatWebSocket::handleNewConnection(const HttpRequestPtr &req,
                                        const WebSocketConnectionPtr &wsConnPtr) {
    auto userId = req->attributes()->get<int64_t>("user_id");
    LOG_INFO << "handleNewConnection userId=" << userId;
    wsConnPtr->setContext(std::make_shared<int64_t>(userId));

    auto& shard = getShard(userId);
    {
        std::unique_lock<std::shared_mutex> lock(shard.mtx);
        auto it = shard.connections.find(userId);
        if (it != shard.connections.end() && it->second->connected()) {
            closeOldConnection(it->second);
        }
        bool inserted = shard.connections.emplace(userId, wsConnPtr).second;
        if (inserted) onlineCount_.fetch_add(1, std::memory_order_relaxed);
    }

    pushOfflineMessages(userId, wsConnPtr);
}


void ChatWebSocket::handleNewMessage(const WebSocketConnectionPtr &wsConnPtr,
                                     std::string &&message,
                                     const WebSocketMessageType &type) {
    if (type != WebSocketMessageType::Text) return;

    int64_t fromUserId = getUserId(wsConnPtr);
    if (fromUserId == -1) {
        wsConnPtr->forceClose();
        return;
    }

    thread_local Json::Reader reader;
    thread_local Json::Value msgJson;
    msgJson.clear();
    if (!reader.parse(message, msgJson)) {
        LOG_WARN << "Invalid JSON from user " << fromUserId;
        return;
    }

    if (!msgJson.isMember("chat_type") || !msgJson["chat_type"].isInt() ||
        !msgJson.isMember("target_id") || !msgJson["target_id"].isInt64() ||
        !msgJson.isMember("msg_type") || !msgJson["msg_type"].isInt()) {
        LOG_WARN << "Missing required fields";
        return;
    }

    int64_t clientTempId = 0;
    if (msgJson.isMember("client_temp_id")) {
        if (msgJson["client_temp_id"].isInt64()) {
            clientTempId = msgJson["client_temp_id"].asInt64();
        } else if (msgJson["client_temp_id"].isString()) {
            clientTempId = std::stoll(msgJson["client_temp_id"].asString());
        }
    }

    int chatType = msgJson["chat_type"].asInt();
    if (chatType == 0) {
        processSingleChat(wsConnPtr, msgJson, fromUserId, clientTempId);
    } else if (chatType == 1) {
        processGroupChat(wsConnPtr, msgJson, fromUserId, clientTempId);
    } else {
        LOG_WARN << "Invalid chat_type";
    }
}

bool ChatWebSocket::sendToUser(int64_t userId, const std::string &message) {
    auto& shard = getShard(userId);
    WebSocketConnectionPtr conn;
    {
        std::shared_lock<std::shared_mutex> lock(shard.mtx);
        auto it = shard.connections.find(userId);
        if (it != shard.connections.end() && it->second->connected()) {
            conn = it->second;
        }
    }
    if (conn) {
        conn->send(message);
        return true;
    }
    return false;
}

void ChatWebSocket::sendAck(int64_t toUserId, int64_t clientTempId, int status) {
    std::string ackStr = buildAck(clientTempId, status);
    sendToUser(toUserId, ackStr);
}

void ChatWebSocket::processSingleChat(const WebSocketConnectionPtr &wsConnPtr,
                                      const Json::Value &msgJson,
                                      int64_t fromUserId,
                                      int64_t clientTempId) {
    int64_t toUserId = msgJson["target_id"].asInt64();
    LOG_INFO << "processSingleChat: from=" << fromUserId << " to=" << toUserId;

    // Send ACK (inline string, no JSON serialization)
    sendAck(fromUserId, clientTempId, 0);

    // Build message payload
    std::string msgStr = fastJson(msgJson);
    msgStr.resize(msgStr.size() - 2);
    int64_t now = time(nullptr);
    msgStr += ",\"from_user_id\":" + std::to_string(fromUserId)
        + ",\"timestamp\":" + std::to_string(now) + "}\n";

    // Try direct delivery
    auto& shard = getShard(toUserId);
    WebSocketConnectionPtr targetConn;
    {
        std::shared_lock<std::shared_mutex> lock(shard.mtx);
        auto it = shard.connections.find(toUserId);
        if (it != shard.connections.end() && it->second->connected()) {
            targetConn = it->second;
        }
    }

    if (targetConn) {
        // Online: deliver directly, backup via Redis
        targetConn->send(msgStr);
        storeOfflineMessage(msgJson, fromUserId, toUserId, 0, toUserId, true);
    } else {
        // Offline: persist to MySQL synchronously (must complete before SELECT on reconnect)
        LOG_INFO << "Saving offline msg for user " << toUserId;
        auto db = app().getDbClient("serverDb");
        std::string content = msgJson.get("content", "").asString();
        std::string mediaUrl = msgJson.get("media_url", "").asString();
        std::string mediaThumb = msgJson.get("media_thumb_url", "").asString();
        int msgType = msgJson.get("msg_type", 0).asInt();
        int64_t mediaSize = msgJson.get("media_size", 0).asInt64();
        int mediaDuration = msgJson.get("media_duration", 0).asInt();
        std::string mediaFormat = msgJson.get("media_format", "").asString();
        try {
            db->execSqlSync(
                "INSERT INTO offline_message (from_user_id,to_user_id,chat_type,target_id,"
                "msg_type,content,media_url,media_thumb_url,media_size,media_duration,"
                "media_format,expire_time) VALUES (?,?,?,?,?,?,?,?,?,?,?,NOW()+INTERVAL 7 DAY)",
                fromUserId, toUserId, 0, toUserId, msgType,
                content, mediaUrl, mediaThumb, mediaSize, mediaDuration, mediaFormat
            );
        } catch (const drogon::orm::DrogonDbException& e) {
            LOG_ERROR << "Sync offline insert failed: " << e.base().what();
        }
    }
}

void ChatWebSocket::processGroupChat(const WebSocketConnectionPtr &wsConnPtr,
                                     const Json::Value &msgJson,
                                     int64_t fromUserId,
                                     int64_t clientTempId) {
    int64_t groupId = msgJson["target_id"].asInt64();

    // Send ACK (inline string, no JSON serialization)
    sendAck(fromUserId, clientTempId, 0);

    auto db = app().getDbClient("serverDb");
    (*db) << "SELECT user_id FROM group_member WHERE group_id = ? AND status = 1"
        >> [this, msgJson, fromUserId, groupId](const Result &r) {
            int64_t now = time(nullptr);
            for (auto &row : r) {
                int64_t memberId = row["user_id"].as<int64_t>();
                if (memberId == fromUserId) continue;

                // Build final message with metadata
                std::string msgStr = fastJson(msgJson);
                msgStr.resize(msgStr.size() - 2);
                msgStr += ",\"from_user_id\":" + std::to_string(fromUserId)
                    + ",\"timestamp\":" + std::to_string(now) + "}\n";

                auto& shard = getShard(memberId);
                WebSocketConnectionPtr targetConn;
                {
                    std::shared_lock<std::shared_mutex> lock(shard.mtx);
                    auto it = shard.connections.find(memberId);
                    if (it != shard.connections.end() && it->second->connected()) {
                        targetConn = it->second;
                    }
                }
                if (targetConn) {
                    targetConn->send(std::move(msgStr));
                    storeOfflineMessage(msgJson, fromUserId, memberId, 1, groupId, true);
                } else {
                    storeOfflineMessage(msgJson, fromUserId, memberId, 1, groupId, false);
                }
            }
        } << groupId;
}

void ChatWebSocket::storeOfflineMessage(const Json::Value &msgJson,
                                        int64_t fromUserId,
                                        int64_t toUserId,
                                        int chatType,
                                        int64_t targetId,
                                        bool isBackup) {
    AsyncMessageWriter::instance().push(fromUserId, toUserId, chatType, targetId, msgJson, isBackup);
}

void ChatWebSocket::handleConnectionClosed(const WebSocketConnectionPtr &wsConnPtr) {
    int64_t userId = getUserId(wsConnPtr);
    if (userId == -1) return;

    bool erased = false;
    {
        auto& shard = getShard(userId);
        std::unique_lock<std::shared_mutex> lock(shard.mtx);
        auto it = shard.connections.find(userId);
        if (it != shard.connections.end() && it->second == wsConnPtr) {
            shard.connections.erase(it);
            erased = true;
        }
    }

    if (erased) onlineCount_.fetch_sub(1, std::memory_order_relaxed);
    LOG_INFO << "User " << userId << " disconnected. Online: " << onlineCount_.load(std::memory_order_relaxed);
}

void ChatWebSocket::pushOfflineMessages(int64_t userId, const WebSocketConnectionPtr &wsConnPtr) {
    LOG_INFO << "pushOffline called for user " << userId;
    auto db = app().getDbClient("serverDb");
    db->execSqlAsync(
        "SELECT id, from_user_id, chat_type, target_id, msg_type, content, "
        "media_url, media_thumb_url, media_size, media_duration, media_format, "
        "UNIX_TIMESTAMP(created_at) as ts "
        "FROM offline_message WHERE to_user_id = ? ORDER BY created_at ASC",
        [wsConnPtr, userId, db](const Result &r) {
            LOG_INFO << "pushOffline: found " << r.size() << " msgs for user " << userId;
            if (r.size() == 0) return;

            for (auto &row : r) {
                Json::Value msg;
                msg["type"] = "offline_msg";
                msg["from_user_id"] = (Json::Int64)row["from_user_id"].as<int64_t>();
                msg["chat_type"] = row["chat_type"].as<int>();
                msg["target_id"] = (Json::Int64)row["target_id"].as<int64_t>();
                msg["msg_type"] = row["msg_type"].as<int>();
                msg["content"] = row["content"].as<std::string>();
                msg["media_url"] = row["media_url"].as<std::string>();
                msg["media_thumb_url"] = row["media_thumb_url"].as<std::string>();
                msg["media_size"] = (Json::Int64)row["media_size"].as<int64_t>();
                msg["media_duration"] = row["media_duration"].as<int>();
                msg["media_format"] = row["media_format"].as<std::string>();
                msg["timestamp"] = (Json::Int64)row["ts"].as<int64_t>();

                std::string msgStr = fastJson(msg);
                wsConnPtr->send(msgStr);
            }

            db->execSqlAsync("DELETE FROM offline_message WHERE to_user_id = ?",
                             [userId](const Result &) {},
                             [](const DrogonDbException &e) {
                                 LOG_ERROR << "Failed to delete offline messages: " << e.base().what();
                             },
                             userId);
        },
        [](const DrogonDbException &e) {
            LOG_ERROR << "Failed to fetch offline messages: " << e.base().what();
        },
        userId
    );
}
