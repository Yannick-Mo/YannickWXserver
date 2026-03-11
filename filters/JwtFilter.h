#pragma once
#include <drogon/HttpFilter.h>

class JwtFilter : public drogon::HttpFilter<JwtFilter> {
public:
    JwtFilter() {}
    void doFilter(const drogon::HttpRequestPtr &req,
                  drogon::FilterCallback &&fcb,
                  drogon::FilterChainCallback &&fccb) override;
};