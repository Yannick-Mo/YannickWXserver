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
    auto userId = req->attributes()->get<int64_t>("user_id");
    wsConnPtr->setContext(std::make_shared<int64_t>(userId));

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = userConnections_.find(userId);
        if (it != userConnections_.end() && it->second->connected()) {
            it->second->forceClose();
        }
        userConnections_[userId] = wsConnPtr;
    }

    LOG_INFO << "User " << userId << " connected. Online: " << userConnections_.size();

    // 上线后推送离线消息
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

    Json::Value msgJson;
    Json::Reader reader;
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

    // 提取 client_temp_id（可选）
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

void ChatWebSocket::sendAck(int64_t toUserId, int64_t clientTempId, int status) {
    Json::Value ack;
    ack["type"] = "msg_ack";
    ack["client_temp_id"] = (Json::Int64)clientTempId;
    ack["status"] = status;   // 0=成功, 1=失败
    std::string ackStr = ack.toStyledString();
    sendToUser(toUserId, ackStr);
}

void ChatWebSocket::processSingleChat(const WebSocketConnectionPtr &wsConnPtr,
                                      const Json::Value &msgJson,
                                      int64_t fromUserId,
                                      int64_t clientTempId) {
    int64_t toUserId = msgJson["target_id"].asInt64();

    // 立即返回 ACK（这里假设总能成功，实际可加入校验后返回失败）
    sendAck(fromUserId, clientTempId, 0);

    // 查找对方在线状态
    WebSocketConnectionPtr targetConn;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        auto it = userConnections_.find(toUserId);
        if (it != userConnections_.end() && it->second->connected()) {
            targetConn = it->second;
        }
    }

    Json::Value forwardMsg = msgJson;
    forwardMsg["from_user_id"] = fromUserId;
    forwardMsg["timestamp"] = (Json::Int64)time(nullptr);
    std::string msgStr = forwardMsg.toStyledString();

    if (targetConn) {
        targetConn->send(msgStr);
    } else {
        storeOfflineMessage(msgJson, fromUserId, toUserId, 0, toUserId);
    }
}

void ChatWebSocket::processGroupChat(const WebSocketConnectionPtr &wsConnPtr,
                                     const Json::Value &msgJson,
                                     int64_t fromUserId,
                                     int64_t clientTempId) {
    int64_t groupId = msgJson["target_id"].asInt64();

    // 立即返回 ACK
    sendAck(fromUserId, clientTempId, 0);

    auto db = app().getDbClient("serverDb");
    db->execSqlAsync(
        "SELECT user_id FROM group_member WHERE group_id = ? AND status = 1",
        [this, msgJson, fromUserId, groupId](const Result &r) {
            std::vector<int64_t> memberIds;
            for (auto &row : r) {
                memberIds.push_back(row["user_id"].as<int64_t>());
            }

            Json::Value forwardMsg = msgJson;
            forwardMsg["from_user_id"] = fromUserId;
            forwardMsg["timestamp"] = (Json::Int64)time(nullptr);
            std::string msgStr = forwardMsg.toStyledString();

            for (int64_t memberId : memberIds) {
                if (memberId == fromUserId) continue;

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
                    storeOfflineMessage(msgJson, fromUserId, memberId, 1, groupId);
                }
            }
        },
        [](const DrogonDbException &e) {
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

    int msgType = msgJson["msg_type"].asInt();
    std::string content = msgJson.get("content", "").asString();
    std::string mediaUrl = msgJson.get("media_url", "").asString();
    std::string thumbUrl = msgJson.get("media_thumb_url", "").asString();
    int64_t mediaSize = msgJson.get("media_size", 0).asInt64();
    int mediaDuration = msgJson.get("media_duration", 0).asInt();
    std::string mediaFormat = msgJson.get("media_format", "").asString();
    auto expireTime = trantor::Date::date().after(7 * 24 * 3600);

    db->execSqlAsync(
        "INSERT INTO offline_message (from_user_id, to_user_id, chat_type, target_id, msg_type, content, "
        "media_url, media_thumb_url, media_size, media_duration, media_format, expire_time) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", 
        [](const Result &r) { LOG_DEBUG << "Offline message stored"; },
        [](const DrogonDbException &e) { LOG_ERROR << "Failed to store offline message: " << e.base().what(); },
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

void ChatWebSocket::pushOfflineMessages(int64_t userId, const WebSocketConnectionPtr &wsConnPtr) {
    auto db = app().getDbClient("serverDb");
    db->execSqlAsync(
        "SELECT id, from_user_id, chat_type, target_id, msg_type, content, "
        "media_url, media_thumb_url, media_size, media_duration, media_format, "
        "UNIX_TIMESTAMP(created_at) as ts "
        "FROM offline_message WHERE to_user_id = ? ORDER BY created_at ASC",
        [wsConnPtr, userId, db](const Result &r) {
            LOG_DEBUG << "Found " << r.size() << " offline messages for user " << userId;
            if (r.size() == 0) return;

            for (auto &row : r) {
                Json::Value msg;
                msg["type"] = "offline_msg";
                msg["from_user_id"] = row["from_user_id"].as<int64_t>();
                msg["chat_type"] = row["chat_type"].as<int>();
                msg["target_id"] = row["target_id"].as<int64_t>();
                msg["msg_type"] = row["msg_type"].as<int>();
                msg["content"] = row["content"].as<std::string>();
                msg["media_url"] = row["media_url"].as<std::string>();
                msg["media_thumb_url"] = row["media_thumb_url"].as<std::string>();
                msg["media_size"] = row["media_size"].as<int64_t>();
                msg["media_duration"] = row["media_duration"].as<int>();
                msg["media_format"] = row["media_format"].as<std::string>();
                msg["timestamp"] = row["ts"].as<int64_t>();

                std::string msgStr = msg.toStyledString();
                LOG_DEBUG << "Sending offline message to user " << userId << ": " << msgStr;
                wsConnPtr->send(msgStr);
            }

            // 推送完成后删除离线消息
            db->execSqlAsync("DELETE FROM offline_message WHERE to_user_id = ?",
                             [userId](const Result &) { LOG_DEBUG << "Offline messages deleted for user " << userId; },
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






