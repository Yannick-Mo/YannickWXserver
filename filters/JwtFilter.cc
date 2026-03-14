#include "JwtFilter.h"
#include <jwt-cpp/jwt.h>
#include <drogon/drogon.h>

using namespace drogon;

void JwtFilter::doFilter(const HttpRequestPtr &req,
                         FilterCallback &&fcb,
                         FilterChainCallback &&fccb) {
    auto auth = req->getHeader("Authorization");
    if (auth.empty() || auth.substr(0, 7) != "Bearer ") {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k401Unauthorized);
        resp->setBody("Missing or invalid token format");
        fcb(resp);
        return;
    }

    std::string token = auth.substr(7);
    auto &custom = app().getCustomConfig();
    std::string secret = custom.get("jwt_secret", "").asString();
    if (secret.empty()) {
        LOG_ERROR << "JWT secret not configured";
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k500InternalServerError);
        resp->setBody("Server configuration error");
        fcb(resp);
        return;
    }

    try {
        auto decoded = jwt::decode(token);
        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{secret})
            .with_issuer("wechat_server");  
        verifier.verify(decoded);

        auto userId = decoded.get_payload_claim("user_id").as_string();
        // 使用 getAttributes()->insert() 存储属性
        req->getAttributes()->insert("user_id", userId);

        fccb();
    } catch (const jwt::error::token_verification_exception &e) {
        LOG_WARN << "JWT verification failed: " << e.what();
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k401Unauthorized);
        resp->setBody("Invalid token");
        fcb(resp);
    } catch (const std::exception &e) {
        LOG_ERROR << "JWT decode error: " << e.what();
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k400BadRequest);
        resp->setBody("Malformed token");
        fcb(resp);
    }
}