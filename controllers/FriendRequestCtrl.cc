#include "FriendRequestCtrl.h"
#include "FriendRequest.h"
#include "Friend.h"
#include "User.h"
#include "ChatWebSocket.h"
#include <drogon/orm/Mapper.h>
#include <json/json.h>
#include <unordered_map>
#include <vector>
#include <exception>

using namespace drogon;
using namespace drogon::orm;
using namespace drogon_model::yaroserverdb;

// 状态枚举
enum class RequestStatus : int8_t {
    PENDING = 0,
    ACCEPTED = 1,
    REJECTED = 2
};
enum class FriendStatus : int8_t {
    NORMAL = 0
};

// 响应工具
namespace ResponseUtils {
    static HttpResponsePtr makeErrorResp(HttpStatusCode code, const std::string &msg) {
        Json::Value ret;
        ret["error"] = msg;
        auto resp = HttpResponse::newHttpJsonResponse(ret);
        resp->setStatusCode(code);
        return resp;
    }
    static std::string toCompactJson(const Json::Value &val) {
        Json::FastWriter writer;
        writer.omitEndingLineFeed();
        return writer.write(val);
    }
}

// 静态工具函数
static bool isAlreadyFriend(int64_t userId, int64_t friendId) {
    try {
        auto db = app().getDbClient("serverDb");
        Mapper<Friend> mapper(db);
        auto cond = Criteria(Friend::Cols::_user_id, CompareOperator::EQ, userId) &&
                    Criteria(Friend::Cols::_friend_id, CompareOperator::EQ, friendId) &&
                    Criteria(Friend::Cols::_status, CompareOperator::EQ, static_cast<int8_t>(FriendStatus::NORMAL));
        return !mapper.findBy(cond).empty();
    } catch (...) {
        return false;
    }
}

static bool hasPendingRequest(int64_t fromUid, int64_t toUid) {
    try {
        auto db = app().getDbClient("serverDb");
        Mapper<FriendRequest> mapper(db);
        auto cond = Criteria(FriendRequest::Cols::_from_user_id, CompareOperator::EQ, fromUid) &&
                    Criteria(FriendRequest::Cols::_to_user_id, CompareOperator::EQ, toUid) &&
                    Criteria(FriendRequest::Cols::_status, CompareOperator::EQ, static_cast<int8_t>(RequestStatus::PENDING));
        return !mapper.findBy(cond).empty();
    } catch (...) {
        return false;
    }
}

static void setFriendFieldsFromMeta(Friend& friendObj, const Json::Value& meta) {
    if (meta.isMember("remark") && meta["remark"].isString())
        friendObj.setRemark(meta["remark"].asString());
    if (meta.isMember("description") && meta["description"].isString())
        friendObj.setDescription(meta["description"].asString());
    if (meta.isMember("phone_note") && meta["phone_note"].isString())
        friendObj.setPhoneNote(meta["phone_note"].asString());
    if (meta.isMember("email_note") && meta["email_note"].isString())
        friendObj.setEmailNote(meta["email_note"].asString());
    if (meta.isMember("source") && meta["source"].isString())
        friendObj.setSource(meta["source"].asString());
    if (meta.isMember("is_starred") && meta["is_starred"].isInt())
        friendObj.setIsStarred(static_cast<int8_t>(meta["is_starred"].asInt()));
    if (meta.isMember("is_blocked") && meta["is_blocked"].isInt())
        friendObj.setIsBlocked(static_cast<int8_t>(meta["is_blocked"].asInt()));
    if (meta.isMember("tags") && (meta["tags"].isObject() || meta["tags"].isArray()))
        friendObj.setTags(ResponseUtils::toCompactJson(meta["tags"]));
}

// 1. 发送好友申请
void FriendRequestCtrl::sendRequest(const HttpRequestPtr &req,
                                    std::function<void(const HttpResponsePtr &)> &&callback) {
    auto json = req->getJsonObject();
    if (!json) {
        callback(ResponseUtils::makeErrorResp(k400BadRequest, "No JSON object"));
        return;
    }

    int64_t fromUid;
    try {
        fromUid = req->getAttributes()->get<int64_t>("user_id");
    } catch (...) {
        callback(ResponseUtils::makeErrorResp(k401Unauthorized, "Unauthorized"));
        return;
    }

    if (!json->isMember("to_user_id") || !(*json)["to_user_id"].isNumeric()) {
        callback(ResponseUtils::makeErrorResp(k400BadRequest, "Missing or invalid to_user_id"));
        return;
    }
    
    int64_t toUid = (*json)["to_user_id"].asInt64();
    if (toUid <= 0 || fromUid == toUid) {
        callback(ResponseUtils::makeErrorResp(k400BadRequest, "Cannot add yourself"));
        return;
    }

    if (isAlreadyFriend(fromUid, toUid)) {
        callback(ResponseUtils::makeErrorResp(k400BadRequest, "Already friends"));
        return;
    }
    if (hasPendingRequest(fromUid, toUid)) {
        callback(ResponseUtils::makeErrorResp(k400BadRequest, "Friend request already sent"));
        return;
    }

    try {
        auto db = app().getDbClient("serverDb");
        Mapper<FriendRequest> mapper(db);
        
        FriendRequest newReq;
        newReq.setFromUserId(fromUid);
        newReq.setToUserId(toUid);
        newReq.setMessage((*json).get("message", "").asString());
        newReq.setStatus(static_cast<int8_t>(RequestStatus::PENDING));
        
        Json::Value meta;
        if (json->isMember("applicant_meta") && (*json)["applicant_meta"].isObject()) {
            meta = (*json)["applicant_meta"];
            newReq.setApplicantMeta(ResponseUtils::toCompactJson(meta));
        }

        mapper.insert(newReq);

        // WebSocket通知（完整保留）
        Json::Value notification;
        notification["type"] = "friend_request";
        notification["data"]["request_id"] = newReq.getValueOfId();
        notification["data"]["from_user_id"] = (Json::Int64)fromUid;
        notification["data"]["message"] = newReq.getValueOfMessage();
        ChatWebSocket::sendToUser(toUid, ResponseUtils::toCompactJson(notification));

        Json::Value ret;
        ret["message"] = "Request sent";
        ret["request_id"] = newReq.getValueOfId();
        callback(HttpResponse::newHttpJsonResponse(ret));
    } catch (const std::exception &e) {
        LOG_ERROR << "Send request error: " << e.what();
        callback(ResponseUtils::makeErrorResp(k500InternalServerError, "Database error"));
    }
}

// 2. 获取待处理申请
// 2. 获取所有好友申请（包含待处理、已同意、已拒绝）
void FriendRequestCtrl::getPendingRequests(const HttpRequestPtr &req,
                                           std::function<void(const HttpResponsePtr &)> &&callback) {
    int64_t userId;
    try {
        userId = req->getAttributes()->get<int64_t>("user_id");
    } catch (...) {
        callback(ResponseUtils::makeErrorResp(k401Unauthorized, "Unauthorized"));
        return;
    }

    try {
        auto db = app().getDbClient("serverDb");
        Mapper<FriendRequest> reqMapper(db);
        
        // 只查询 我作为接收方 的所有申请，不限制 status（0=待处理 1=同意 2=拒绝）
        auto cond = Criteria(FriendRequest::Cols::_to_user_id, CompareOperator::EQ, userId);

        // 按时间倒序返回全部
        auto requests = reqMapper.orderBy(FriendRequest::Cols::_created_at, SortOrder::DESC).findBy(cond);
        if (requests.empty()) {
            callback(HttpResponse::newHttpJsonResponse(Json::arrayValue));
            return;
        }

        // 查询申请人信息
        std::vector<int64_t> fromUids;
        for (const auto &r : requests) fromUids.push_back(r.getValueOfFromUserId());

        Mapper<User> userMapper(db);
        auto users = userMapper.findBy(Criteria(User::Cols::_id, CompareOperator::In, fromUids));
        std::unordered_map<int64_t, User> userMap;
        for (const auto &u : users) userMap[u.getValueOfId()] = u;

        // 组装返回数据
        Json::Value result(Json::arrayValue);
        for (const auto &r : requests) {
            Json::Value item;
            item["id"] = (Json::Int64)r.getValueOfId();
            item["from_user_id"] = (Json::Int64)r.getValueOfFromUserId();
            item["message"] = r.getValueOfMessage();
            item["status"] = r.getValueOfStatus(); // 0 1 2 都会返回
            
            if(r.getCreatedAt()){
                item["created_at"] = r.getCreatedAt()->toDbStringLocal();
            }
            
            auto it = userMap.find(r.getValueOfFromUserId());
            if (it != userMap.end()) {
                item["from_nickname"] = it->second.getValueOfNickname();
                item["from_avatar"] = it->second.getValueOfAvatarUrl();
                item["from_account"] = it->second.getValueOfUsername();
            }
            result.append(item);
        }
        callback(HttpResponse::newHttpJsonResponse(result));
    } catch (const std::exception &e) {
        LOG_ERROR << "Query requests error: " << e.what();
        callback(ResponseUtils::makeErrorResp(k500InternalServerError, "Database error"));
    }
}

// 3. 处理好友申请
void FriendRequestCtrl::processRequest(const HttpRequestPtr &req,
                                       std::function<void(const HttpResponsePtr &)> &&callback,
                                       int64_t requestId) {
    auto json = req->getJsonObject();
    if (!json || !json->isMember("status") || !(*json)["status"].isInt()) {
        callback(ResponseUtils::makeErrorResp(k400BadRequest, "Missing status field"));
        return;
    }

    int8_t newStatus = static_cast<int8_t>((*json)["status"].asInt());
    if (newStatus != 1 && newStatus != 2) {
        callback(ResponseUtils::makeErrorResp(k400BadRequest, "Invalid status"));
        return;
    }

    int64_t currentUid;
    try {
        currentUid = req->getAttributes()->get<int64_t>("user_id");
    } catch (...) {
        callback(ResponseUtils::makeErrorResp(k401Unauthorized, "Unauthorized"));
        return;
    }

    try {
        auto db = app().getDbClient("serverDb");
        auto trans = db->newTransaction();
        Mapper<FriendRequest> reqMapper(trans);

        auto request = reqMapper.findByPrimaryKey(requestId);
        if (request.getValueOfToUserId() != currentUid) {
            callback(ResponseUtils::makeErrorResp(k403Forbidden, "Permission denied"));
            return;
        }
        if (request.getValueOfStatus() != static_cast<int8_t>(RequestStatus::PENDING)) {
            callback(ResponseUtils::makeErrorResp(k400BadRequest, "Request already processed"));
            return;
        }

        // 更新申请状态
        request.setStatus(newStatus);
        reqMapper.update(request);

        // 同意申请，解析附加数据
        if (newStatus == static_cast<int8_t>(RequestStatus::ACCEPTED)) {
            // 1. 解析申请人预设的 meta（写入 f1，即申请人视角）
            Json::Value applicantMeta;
            const std::string& metaStr = request.getValueOfApplicantMeta();
            if (!metaStr.empty()) {
                Json::Reader reader;
                reader.parse(metaStr, applicantMeta);
            }

            Mapper<Friend> friendMapper(trans);
            Friend f1, f2;

            // f1: 申请人看被申请人的记录
            f1.setUserId(request.getValueOfFromUserId());
            f1.setFriendId(request.getValueOfToUserId());
            f1.setStatus(static_cast<int8_t>(FriendStatus::NORMAL));
            setFriendFieldsFromMeta(f1, applicantMeta);

            // 2. 解析被申请人本次提交的 meta（写入 f2，即被申请人视角）
            Json::Value responderMeta;
            if (json->isMember("meta") && (*json)["meta"].isObject()) {
                responderMeta = (*json)["meta"];
            }

            f2.setUserId(request.getValueOfToUserId());
            f2.setFriendId(request.getValueOfFromUserId());
            f2.setStatus(static_cast<int8_t>(FriendStatus::NORMAL));
            setFriendFieldsFromMeta(f2, responderMeta);

            friendMapper.insert(f1);
            friendMapper.insert(f2);
        }

        // WebSocket 推送
        Json::Value notify;
        notify["type"] = newStatus == 1 ? "friend_request_accepted" : "friend_request_rejected";
        notify["data"]["request_id"] = requestId;
        ChatWebSocket::sendToUser(request.getValueOfFromUserId(), ResponseUtils::toCompactJson(notify));

        Json::Value ret;
        ret["message"] = "Request processed";
        callback(HttpResponse::newHttpJsonResponse(ret));
    } catch (const std::exception &e) {
        LOG_ERROR << "Process request error: " << e.what();
        callback(ResponseUtils::makeErrorResp(k500InternalServerError, "Database error"));
    }
}












