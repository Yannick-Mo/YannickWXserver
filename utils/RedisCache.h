#pragma once
#include <hiredis/hiredis.h>
#include <string>
#include <mutex>
#include <vector>
#include <memory>
#include <atomic>
#include <queue>
#include <thread>
#include <functional>
#include <condition_variable>
#include <drogon/drogon.h>

class RedisCache {
public:
    static RedisCache& instance() {
        static RedisCache inst;
        return inst;
    }

    void init(const std::string& host, int port, int poolSize = 4) {
        if (!connections_.empty()) return;
        struct timeval tv = {1, 0};
        for (int i = 0; i < poolSize; ++i) {
            auto* c = redisConnectWithTimeout(host.c_str(), port, tv);
            if (!c || c->err) {
                LOG_ERROR << "Redis conn " << i << ": " << (c ? c->errstr : "alloc");
                if (c) redisFree(c);
                continue;
            }
            redisSetTimeout(c, tv);
            connections_.push_back(c);
            connMutexes_.emplace_back(std::make_unique<std::mutex>());
        }
        LOG_INFO << "RedisCache: " << connections_.size() << "/" << poolSize << " ready";
        startWorker();
    }

    ~RedisCache() {
        stopWorker();
        for (auto* c : connections_) redisFree(c);
    }

    bool setex(const std::string& key, int ttl, const std::string& value) {
        auto idx = getConnIdx();
        if (idx < 0) return false;
        std::lock_guard<std::mutex> lk(*connMutexes_[idx]);
        auto* reply = (redisReply*)redisCommand(connections_[idx], "SETEX %s %d %s",
            key.c_str(), ttl, value.c_str());
        bool ok = reply && reply->type == REDIS_REPLY_STATUS;
        if (reply) freeReplyObject(reply);
        if (!ok && connections_[idx]->err) {
            reconnectConn(idx);
            reply = (redisReply*)redisCommand(connections_[idx], "SETEX %s %d %s",
                key.c_str(), ttl, value.c_str());
            ok = reply && reply->type == REDIS_REPLY_STATUS;
            if (reply) freeReplyObject(reply);
            return ok;
        }
        return ok;
    }

    std::string get(const std::string& key) {
        auto idx = getConnIdx();
        if (idx < 0) return "";
        std::lock_guard<std::mutex> lk(*connMutexes_[idx]);
        auto* reply = (redisReply*)redisCommand(connections_[idx], "GET %s", key.c_str());
        std::string val;
        if (reply && reply->type == REDIS_REPLY_STRING) val = reply->str;
        if (reply) freeReplyObject(reply);
        if (val.empty() && connections_[idx]->err) {
            reconnectConn(idx);
            reply = (redisReply*)redisCommand(connections_[idx], "GET %s", key.c_str());
            if (reply && reply->type == REDIS_REPLY_STRING) val = reply->str;
            if (reply) freeReplyObject(reply);
        }
        return val;
    }

    void setexAsync(const std::string& key, int ttl, const std::string& value) {
        {
            std::lock_guard<std::mutex> lk(queueMtx_);
            queue_.push([this, key, ttl, value]() { setex(key, ttl, value); });
        }
        cv_.notify_one();
    }

private:
    int getConnIdx() {
        if (connections_.empty()) return -1;
        size_t idx = next_.fetch_add(1, std::memory_order_relaxed) % connections_.size();
        return (int)idx;
    }

    bool reconnectConn(int idx) {
        if (idx < 0 || idx >= (int)connections_.size()) return false;
        auto& cfg = drogon::app().getCustomConfig();
        std::string host = cfg.get("redis_host", "127.0.0.1").asString();
        int port = cfg.get("redis_port", 6379).asInt();
        struct timeval tv = {1, 0};
        auto* c = redisConnectWithTimeout(host.c_str(), port, tv);
        if (c && !c->err) {
            redisSetTimeout(c, tv);
            if (connections_[idx]) redisFree(connections_[idx]);
            connections_[idx] = c;
            return true;
        }
        if (c) redisFree(c);
        return false;
    }

    void startWorker() {
        running_ = true;
        worker_ = std::thread([this]() {
            while (running_) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lk(queueMtx_);
                    cv_.wait_for(lk, std::chrono::seconds(1), [this]() {
                        return !queue_.empty() || !running_;
                    });
                    if (!running_) break;
                    if (queue_.empty()) continue;
                    task = std::move(queue_.front());
                    queue_.pop();
                }
                task();
            }
        });
    }

    void stopWorker() {
        running_ = false;
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    std::vector<redisContext*> connections_;
    std::vector<std::unique_ptr<std::mutex>> connMutexes_;
    std::atomic<size_t> next_{0};

    std::queue<std::function<void()>> queue_;
    std::mutex queueMtx_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> running_{false};
};
