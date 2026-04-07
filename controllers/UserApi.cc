#include "UserApi.h"
#include <drogon/HttpResponse.h>
#include <drogon/HttpRequest.h>
#include <drogon/orm/Mapper.h>
#include <drogon/orm/Exception.h>
#include <json/json.h>
#include "User.h"
#include <drogon/orm/Criteria.h>
#include "Group.h"
#include "GroupMember.h"

using namespace drogon;
using namespace drogon::orm;
using namespace drogon_model::yaroserverdb;


void UserApi::getProfile(const HttpRequestPtr &req,
                         std::function<void(const HttpResponsePtr &)> &&callback) {
    // 获取用户 ID
    int64_t userId = 0;
    try {
        userId = req->getAttributes()->get<int64_t>("user_id");
    } catch (const std::bad_any_cast &e) {
        LOG_ERROR << "getProfile: bad_any_cast: " << e.what();
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setBody("Internal error: user_id type mismatch");
        callback(resp);
        return;
    } catch (const std::exception &e) {
        LOG_ERROR << "getProfile: exception while getting user_id: " << e.what();
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setBody("Internal error");
        callback(resp);
        return;
    }

    // 获取数据库客户端
    auto dbClient = app().getDbClient("serverDb");
    if (!dbClient) {
        LOG_ERROR << "getProfile: database client is null!";
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setBody("Database connection error");
        callback(resp);
        return;
    }

    Mapper<User> mapper(dbClient);
    mapper.findByPrimaryKey(
        userId,
        [callback](const User &user) {
            Json::Value ret;
            ret["user_id"] = user.getValueOfId();
            ret["account"] = user.getValueOfUsername();
            ret["nickname"] = user.getValueOfNickname();
            ret["avatar"] = user.getValueOfAvatarUrl();
            ret["profile_cover"] = user.getValueOfCoverUrl();
            ret["avatar_local_path"] = "";
            ret["gender"] = user.getValueOfGender();
            ret["region"] = user.getValueOfRegion();
            ret["signature"] = user.getValueOfSignature();
            ret["is_current"] = 1;

            auto resp = HttpResponse::newHttpJsonResponse(ret);
            callback(resp);
        },
        [callback](const DrogonDbException &e) {
            LOG_ERROR << "getProfile: database error: " << e.base().what();
            auto resp = HttpResponse::newHttpResponse();
            if (dynamic_cast<const UnexpectedRows *>(&e.base())) {
                resp->setStatusCode(k404NotFound);
                resp->setBody("User not found");
            } else {
                resp->setStatusCode(k500InternalServerError);
                resp->setBody("Database error");
            }
            callback(resp);
        });
}

void UserApi::getAllGroupMembers(const HttpRequestPtr &req,
                                 std::function<void(const HttpResponsePtr &)> &&callback) {
    // 1. 获取当前用户ID
    int64_t userId = 0;
    try {
        userId = req->getAttributes()->get<int64_t>("user_id");
    } catch (const std::bad_any_cast &e) {
        LOG_ERROR << "getAllGroupMembers: bad_any_cast: " << e.what();
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setBody("Internal error: user_id type mismatch");
        callback(resp);
        return;
    }

    // 2. 获取数据库客户端
    auto dbClient = app().getDbClient("serverDb");
    if (!dbClient) {
        LOG_ERROR << "getAllGroupMembers: database client is null!";
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setBody("Database connection error");
        callback(resp);
        return;
    }

    // 3. 查询当前用户加入的所有群组ID（通过 group_member 表）
    Mapper<GroupMember> memberMapper(dbClient);
    Criteria memberCriteria(GroupMember::Cols::_user_id, CompareOperator::EQ, userId);
    memberMapper.findBy(memberCriteria,
        [dbClient, callback, userId](const std::vector<GroupMember> &userMembers) {
            if (userMembers.empty()) {
                // 用户未加入任何群组
                Json::Value result(Json::arrayValue);
                callback(HttpResponse::newHttpJsonResponse(result));
                return;
            }

            // 提取群组ID列表
            std::vector<int64_t> groupIds;
            for (const auto &member : userMembers) {
                groupIds.push_back(member.getValueOfGroupId());
            }

            // 4. 查询群组基本信息
            Mapper<Group> groupMapper(dbClient);
            Criteria groupCriteria(Group::Cols::_id, CompareOperator::In, groupIds);
            groupMapper.findBy(groupCriteria,
                [dbClient, callback, groupIds](const std::vector<Group> &groups) {
                    // 5. 查询这些群组的所有成员
                    Mapper<GroupMember> memberMapper(dbClient);
                    Criteria allMembersCriteria(GroupMember::Cols::_group_id, CompareOperator::In, groupIds);
                    memberMapper.findBy(allMembersCriteria,
                        [dbClient, callback, groups](const std::vector<GroupMember> &allMembers) {
                            // 6. 收集所有成员的用户ID
                            std::vector<int64_t> userIds;
                            for (const auto &member : allMembers) {
                                userIds.push_back(member.getValueOfUserId());
                            }

                            // 7. 批量查询用户信息（头像、全局昵称等）
                            if (userIds.empty()) {
                                // 没有成员（理论上不应发生），直接返回群信息，成员列表为空
                                Json::Value result(Json::arrayValue);
                                for (const auto &group : groups) {
                                    Json::Value groupObj;
                                    groupObj["group_id"] = group.getValueOfId();
                                    groupObj["group_name"] = group.getValueOfGroupName();
                                    groupObj["avatar"] = group.getValueOfAvatarUrl();
                                    groupObj["announcement"] = group.getValueOfAnnouncement();
                                    groupObj["introduction"] = group.getValueOfIntroduction();
                                    groupObj["max_members"] = group.getValueOfMaxMembers();
                                    groupObj["owner_id"] = group.getValueOfOwnerId();
                                    groupObj["status"] = group.getValueOfStatus();
                                    groupObj["members"] = Json::Value(Json::arrayValue);
                                    result.append(groupObj);
                                }
                                callback(HttpResponse::newHttpJsonResponse(result));
                                return;
                            }

                            Mapper<User> userMapper(dbClient);
                            Criteria userCriteria(User::Cols::_id, CompareOperator::In, userIds);
                            userMapper.findBy(userCriteria,
                                [callback, groups, allMembers](const std::vector<User> &users) {
                                    // 8. 构建 user_id -> User 的映射
                                    std::unordered_map<int64_t, User> userMap;
                                    for (const auto &user : users) {
                                        userMap[user.getValueOfId()] = user;
                                    }

                                    // 9. 按群组组织成员
                                    // 建立 group_id -> 成员列表的映射
                                    std::unordered_map<int64_t, std::vector<Json::Value>> groupMembersMap;
                                    for (const auto &member : allMembers) {
                                        int64_t gid = member.getValueOfGroupId();
                                        Json::Value memberObj;
                                        // ===== 修改点：群内昵称 =====
                                        memberObj["nickname"] = member.getValueOfNickname(); // 群内昵称
                                        memberObj["user_id"] = member.getValueOfUserId();
                                        memberObj["role"] = member.getValueOfRole();
                                        memberObj["join_time"] = member.getJoinTime() ? member.getJoinTime()->toDbStringLocal() : "";
                                        memberObj["status"] = member.getValueOfStatus();
                                        // 从 User 表中补充全局信息（不覆盖群内昵称）
                                        auto it = userMap.find(member.getValueOfUserId());
                                        if (it != userMap.end()) {
                                            memberObj["global_nickname"] = it->second.getValueOfNickname();
                                            memberObj["avatar"] = it->second.getValueOfAvatarUrl();
                                            memberObj["account"] = it->second.getValueOfUsername();
                                        } else {
                                            memberObj["global_nickname"] = "";
                                            memberObj["avatar"] = "";
                                            memberObj["account"] = "";
                                        }
                                        groupMembersMap[gid].push_back(std::move(memberObj));
                                    }

                                    // 10. 构造最终响应
                                    Json::Value result(Json::arrayValue);
                                    for (const auto &group : groups) {
                                        int64_t gid = group.getValueOfId();
                                        Json::Value groupObj;
                                        groupObj["group_id"] = gid;
                                        groupObj["group_name"] = group.getValueOfGroupName();
                                        groupObj["avatar"] = group.getValueOfAvatarUrl();
                                        groupObj["announcement"] = group.getValueOfAnnouncement();
                                        groupObj["introduction"] = group.getValueOfIntroduction();
                                        groupObj["max_members"] = group.getValueOfMaxMembers();
                                        groupObj["owner_id"] = group.getValueOfOwnerId();
                                        groupObj["status"] = group.getValueOfStatus();

                                        auto it = groupMembersMap.find(gid);
                                        if (it != groupMembersMap.end()) {
                                            Json::Value membersArray(Json::arrayValue);
                                            for (const auto &member : it->second) {
                                                membersArray.append(member);
                                            }
                                            groupObj["members"] = membersArray;
                                        } else {
                                            groupObj["members"] = Json::Value(Json::arrayValue);
                                        }
                                        result.append(groupObj);
                                    }

                                    auto resp = HttpResponse::newHttpJsonResponse(result);
                                    callback(resp);
                                },
                                [callback](const DrogonDbException &e) {
                                    LOG_ERROR << "getAllGroupMembers: query users failed: " << e.base().what();
                                    auto resp = HttpResponse::newHttpResponse();
                                    resp->setStatusCode(k500InternalServerError);
                                    resp->setBody("Database error");
                                    callback(resp);
                                });
                        },
                        [callback](const DrogonDbException &e) {
                            LOG_ERROR << "getAllGroupMembers: query all members failed: " << e.base().what();
                            auto resp = HttpResponse::newHttpResponse();
                            resp->setStatusCode(k500InternalServerError);
                            resp->setBody("Database error");
                            callback(resp);
                        });
                },
                [callback](const DrogonDbException &e) {
                    LOG_ERROR << "getAllGroupMembers: query groups failed: " << e.base().what();
                    auto resp = HttpResponse::newHttpResponse();
                    resp->setStatusCode(k500InternalServerError);
                    resp->setBody("Database error");
                    callback(resp);
                });
        },
        [callback](const DrogonDbException &e) {
            LOG_ERROR << "getAllGroupMembers: query user's groups failed: " << e.base().what();
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k500InternalServerError);
            resp->setBody("Database error");
            callback(resp);
        });
}

void UserApi::updateProfile(const HttpRequestPtr &req,
                            std::function<void(const HttpResponsePtr &)> &&callback) {
    // 获取用户 ID
    int64_t userId = 0;
    try {
        userId = req->getAttributes()->get<int64_t>("user_id");
    } catch (const std::bad_any_cast &e) {
        LOG_ERROR << "updateProfile: bad_any_cast: " << e.what();
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setBody("Internal error: user_id type mismatch");
        callback(resp);
        return;
    } catch (const std::exception &e) {
        LOG_ERROR << "updateProfile: exception while getting user_id: " << e.what();
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setBody("Internal error");
        callback(resp);
        return;
    }

    // 解析 JSON
    auto json = req->getJsonObject();
    if (!json) {
        LOG_WARN << "updateProfile: invalid JSON body";
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k400BadRequest);
        resp->setBody("Invalid JSON");
        callback(resp);
        return;
    }

    // 获取数据库客户端
    auto dbClient = app().getDbClient("serverDb");
    if (!dbClient) {
        LOG_ERROR << "updateProfile: database client is null!";
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setBody("Database connection error");
        callback(resp);
        return;
    }

    Mapper<User> mapper(dbClient);
    mapper.findByPrimaryKey(
        userId,
        [json, callback, userId](User user) {
            bool updated = false;

            if (json->isMember("nickname")) {
                user.setNickname((*json)["nickname"].asString());
                updated = true;
            }
            if (json->isMember("avatar")) {
                user.setAvatarUrl((*json)["avatar"].asString());
                updated = true;
            }
            if (json->isMember("profile_cover")) {
                user.setCoverUrl((*json)["profile_cover"].asString());
                updated = true;
            }
            if (json->isMember("gender")) {
                user.setGender((*json)["gender"].asInt());
                updated = true;
            }
            if (json->isMember("region")) {
                user.setRegion((*json)["region"].asString());
                updated = true;
            }
            if (json->isMember("signature")) {
                user.setSignature((*json)["signature"].asString());
                updated = true;
            }
            if (json->isMember("account")) {
                user.setUsername((*json)["account"].asString());
                updated = true;
            }
            if (json->isMember("email")) {
                user.setEmail((*json)["email"].asString());
                updated = true;
            }

            if (!updated) {
                Json::Value ret;
                ret["status"] = "ok";
                ret["message"] = "No fields to update";
                callback(HttpResponse::newHttpJsonResponse(ret));
                return;
            }

            Mapper<User> updater(app().getDbClient("serverDb"));
            updater.update(user,
                [callback, userId](size_t affected) {
                    Json::Value ret;
                    if (affected > 0) {
                        ret["status"] = "ok";
                    } else {
                        ret["status"] = "error";
                        ret["message"] = "No rows updated";
                    }
                    ret["user_id"] = userId;
                    callback(HttpResponse::newHttpJsonResponse(ret));
                },
                [callback](const DrogonDbException &e) {
                    LOG_ERROR << "updateProfile: update failed: " << e.base().what();
                    auto resp = HttpResponse::newHttpResponse();
                    resp->setStatusCode(k500InternalServerError);
                    resp->setBody("Update failed");
                    callback(resp);
                });
        },
        [callback](const DrogonDbException &e) {
            LOG_ERROR << "updateProfile: user not found: " << e.base().what();
            auto resp = HttpResponse::newHttpResponse();
            if (dynamic_cast<const UnexpectedRows *>(&e.base())) {
                resp->setStatusCode(k404NotFound);
                resp->setBody("User not found");
            } else {
                resp->setStatusCode(k500InternalServerError);
                resp->setBody("Database error");
            }
            callback(resp);
        });
}

void UserApi::searchUsers(const HttpRequestPtr &req,
                          std::function<void(const HttpResponsePtr &)> &&callback) {
    auto keyword = req->getParameter("keyword");
    if (keyword.empty()) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k400BadRequest);
        resp->setBody("Missing keyword");
        callback(resp);
        return;
    }

    auto dbClient = app().getDbClient("serverDb");
    if (!dbClient) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setBody("Database connection error");
        callback(resp);
        return;
    }

    Mapper<User> mapper(dbClient);

    // 精准搜索：仅匹配 用户名(账号)、手机号
    Criteria nameCond(User::Cols::_username, CompareOperator::EQ, keyword);
    Criteria phoneCond(User::Cols::_phone, CompareOperator::EQ, keyword);
    
    // 组合 OR 条件：仅用户名 或 手机号 精准匹配
    Criteria cond = nameCond || phoneCond;

    // 分页参数
    int page = 1, size = 20;
    try {
        if (req->getParameter("page") != "") page = std::stoi(req->getParameter("page"));
        if (req->getParameter("size") != "") size = std::stoi(req->getParameter("size"));
        // 可选：限制最大分页大小
        if (size > 100) size = 100;
    } catch (...) {}

    mapper.orderBy(User::Cols::_id, SortOrder::ASC)
         .limit(size)
         .offset((page - 1) * size)
         .findBy(cond,
            [callback](const std::vector<User> &users) {
                Json::Value result(Json::arrayValue);
                for (const auto &user : users) {
                    Json::Value item;
                    item["user_id"] = user.getValueOfId();
                    item["account"] = user.getValueOfUsername();
                    item["nickname"] = user.getValueOfNickname();
                    item["avatar"] = user.getValueOfAvatarUrl();
                    item["region"] = user.getValueOfRegion();
                    // 可根据需要返回其他字段，但避免返回敏感信息如密码哈希
                    result.append(item);
                }
                auto resp = HttpResponse::newHttpJsonResponse(result);
                callback(resp);
            },
            [callback](const DrogonDbException &e) {
                LOG_ERROR << "searchUsers: database error: " << e.base().what();
                auto resp = HttpResponse::newHttpResponse();
                resp->setStatusCode(k500InternalServerError);
                resp->setBody("Database error");
                callback(resp);
            });
}






