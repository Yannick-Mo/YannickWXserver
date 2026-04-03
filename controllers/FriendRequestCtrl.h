#pragma once

#include <drogon/HttpController.h>

class FriendRequestCtrl : public drogon::HttpController<FriendRequestCtrl> {
public:
    METHOD_LIST_BEGIN
        // 发送好友申请
        ADD_METHOD_TO(FriendRequestCtrl::sendRequest, "/api/friend/request", drogon::Post, "JwtFilter");
        // 获取待处理申请列表
        ADD_METHOD_TO(FriendRequestCtrl::getPendingRequests, "/api/friend/request/pending", drogon::Get, "JwtFilter");
        // 处理申请（同意/拒绝）
        ADD_METHOD_TO(FriendRequestCtrl::processRequest, "/api/friend/request/{1}", drogon::Put, "JwtFilter");
    METHOD_LIST_END

    void sendRequest(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void getPendingRequests(const drogon::HttpRequestPtr &req,
                            std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    void processRequest(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                        int64_t requestId);
};