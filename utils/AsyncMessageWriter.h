#pragma once
#include <hiredis/hiredis.h>
#include <json/json.h>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>
#include <drogon/drogon.h>

class AsyncMessageWriter {
public:
    static AsyncMessageWriter& instance() {
        static AsyncMessageWriter inst;
        return inst;
    }

    void push(int64_t fromUserId, int64_t toUserId, int chatType, int64_t targetId,
              const Json::Value &msgJson, bool isBackup = false)
    {
        thread_local Json::FastWriter writer;
        char prefix = isBackup ? 'B' : 'O';
        std::string entry = prefix + std::string("|")
            + std::to_string(fromUserId) + "|"
            + std::to_string(toUserId) + "|"
            + std::to_string(chatType) + "|"
            + std::to_string(targetId) + "|"
            + writer.write(msgJson);
        {
            std::lock_guard<std::mutex> lk(queueMtx_);
            msgQueue_.push(std::move(entry));
        }
        queueCv_.notify_one();
    }

    void start() {
        if (running_) return;
        running_ = true;
        auto& cfg = drogon::app().getCustomConfig();
        redisHost_ = cfg.get("redis_host", "127.0.0.1").asString();
        redisPort_ = cfg.get("redis_port", 6379).asInt();
        batchSize_ = cfg.get("async_writer_batch", 100).asInt();
        connectFlusher();
        connectWorker();
        worker_ = std::thread([this]() { run(); });
        startFlusher();
    }

    void stop() {
        running_ = false;
        flusherRunning_ = false;
        queueCv_.notify_one();
        // Push sentinel to wake worker from BRPOP
        redisContext* c = redisConnect(redisHost_.c_str(), redisPort_);
        if (c) {
            redisCommand(c, "RPUSH msg_queue __SHUTDOWN__");
            redisFree(c);
        }
        if (worker_.joinable()) worker_.join();
        if (flusher_.joinable()) flusher_.join();
        if (flusherRedis_) { redisFree(flusherRedis_); flusherRedis_ = nullptr; }
        if (workerRedis_) { redisFree(workerRedis_); workerRedis_ = nullptr; }
    }

    void setRedisCtx(redisContext* ctx) { flusherRedis_ = ctx; }

private:
    AsyncMessageWriter() : running_(false), flusherRedis_(nullptr), workerRedis_(nullptr) {}
    ~AsyncMessageWriter() { stop(); }

    std::string redisHost_;
    int redisPort_;
    int batchSize_ = 100;

    void connectFlusher() {
        if (flusherRedis_) return;
        struct timeval tv = {3, 0};
        flusherRedis_ = redisConnectWithTimeout(redisHost_.c_str(), redisPort_, tv);
        if (flusherRedis_ && flusherRedis_->err) {
            LOG_ERROR << "Flusher Redis connect failed: " << flusherRedis_->errstr;
            redisFree(flusherRedis_); flusherRedis_ = nullptr;
        }
    }

    void connectWorker() {
        if (workerRedis_) return;
        struct timeval tv = {3, 0};
        workerRedis_ = redisConnectWithTimeout(redisHost_.c_str(), redisPort_, tv);
        if (workerRedis_ && workerRedis_->err) {
            LOG_ERROR << "Worker Redis connect failed: " << workerRedis_->errstr;
            redisFree(workerRedis_); workerRedis_ = nullptr;
        }
    }

    void startFlusher() {
        if (flusherRunning_) return;
        flusherRunning_ = true;
        flusher_ = std::thread([this]() {
            while (flusherRunning_) {
                std::vector<std::string> batch;
                {
                    std::unique_lock<std::mutex> lk(queueMtx_);
                    queueCv_.wait_for(lk, std::chrono::milliseconds(20), [this]() {
                        return !msgQueue_.empty() || !flusherRunning_;
                    });
                    if (!flusherRunning_) break;
                    while (!msgQueue_.empty() && batch.size() < (size_t)batchSize_) {
                        batch.push_back(std::move(msgQueue_.front()));
                        msgQueue_.pop();
                    }
                }
                if (batch.empty()) continue;

                if (!flusherRedis_) { connectFlusher(); if (!flusherRedis_) continue; }

                // Pipeline all RPUSH commands, then read all replies
                for (auto& entry : batch) {
                    redisAppendCommand(flusherRedis_, "RPUSH msg_queue %s", entry.c_str());
                }
                for (size_t i = 0; i < batch.size(); ++i) {
                    redisReply* r = nullptr;
                    redisGetReply(flusherRedis_, (void**)&r);
                    if (r) freeReplyObject(r);
                }
            }
        });
    }

    void run() {
        auto db = drogon::app().getDbClient("serverDb");
        const std::string POP_LUA = R"(
            local msgs = redis.call('LRANGE', KEYS[1], 0, ARGV[1]-1)
            if #msgs > 0 then redis.call('LTRIM', KEYS[1], #msgs, -1) end
            return msgs
        )";

        while (running_) {
            if (!workerRedis_) { connectWorker(); std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }

            redisReply* reply = (redisReply*)redisCommand(workerRedis_,
                "BRPOP msg_queue 1");
            if (!reply) {
                if (workerRedis_ && workerRedis_->err) {
                    redisFree(workerRedis_); workerRedis_ = nullptr;
                }
                continue;
            }

            if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 2) {
                // Process the BRPOP'd message first
                if (reply->element[1]->type == REDIS_REPLY_STRING) {
                    processOne(db, reply->element[1]->str);
                }
                freeReplyObject(reply);

                reply = (redisReply*)redisCommand(workerRedis_,
                    "EVAL %s 1 msg_queue %d", POP_LUA.c_str(), batchSize_);
                if (!reply) {
                    if (workerRedis_ && workerRedis_->err) {
                        redisFree(workerRedis_); workerRedis_ = nullptr;
                    }
                    continue;
                }

                if (reply->type == REDIS_REPLY_ARRAY) {
                    for (size_t i = 0; i < reply->elements; ++i) {
                        if (reply->element[i]->type == REDIS_REPLY_STRING) {
                            processOne(db, reply->element[i]->str);
                        }
                    }
                    freeReplyObject(reply);
                    continue;
                }
            }
            if (reply) freeReplyObject(reply);
        }
    }

    void processOne(const drogon::orm::DbClientPtr& db, const std::string& entry) {
        if (entry.empty() || entry.size() < 2) return;
        bool isBackup = (entry[0] == 'B');
        size_t start = 2;

        size_t p1 = entry.find('|', start);
        if (p1 == std::string::npos) return;
        size_t p2 = entry.find('|', p1 + 1);
        if (p2 == std::string::npos) return;
        size_t p3 = entry.find('|', p2 + 1);
        if (p3 == std::string::npos) return;
        size_t p4 = entry.find('|', p3 + 1);
        if (p4 == std::string::npos) return;

        int64_t fromUserId = std::stoll(entry.substr(start, p1 - start));
        int64_t toUserId = std::stoll(entry.substr(p1 + 1, p2 - p1 - 1));
        int chatType = std::stoi(entry.substr(p2 + 1, p3 - p2 - 1));
        int64_t targetId = std::stoll(entry.substr(p3 + 1, p4 - p3 - 1));
        std::string_view jsonSv(entry.data() + p4 + 1, entry.size() - p4 - 1);

        if (isBackup) return;

        thread_local Json::Reader reader;
        thread_local Json::Value msgJson;
        msgJson.clear();
        if (!reader.parse(jsonSv.data(), jsonSv.data() + jsonSv.size(), msgJson)) return;

        int msgType = msgJson.get("msg_type", 0).asInt();
        std::string content = msgJson.get("content", "").asString();
        std::string mediaUrl = msgJson.get("media_url", "").asString();
        std::string thumbUrl = msgJson.get("media_thumb_url", "").asString();
        int64_t mediaSize = msgJson.get("media_size", 0).asInt64();
        int mediaDuration = msgJson.get("media_duration", 0).asInt();
        std::string mediaFormat = msgJson.get("media_format", "").asString();

        // Queue DB INSERT to IO thread (DbClient not thread-safe from worker thread)
        int64_t capturedTargetId = targetId;
        drogon::app().getLoop()->queueInLoop([capturedTargetId, fromUserId, toUserId, chatType, msgType,
            content, mediaUrl, thumbUrl, mediaSize, mediaDuration, mediaFormat]() {
            LOG_DEBUG << "AsyncMessageWriter: INSERT offline msg to_user=" << toUserId;
            auto db = drogon::app().getDbClient("serverDb");
            db->execSqlAsync(
                "INSERT INTO offline_message (from_user_id,to_user_id,chat_type,target_id,"
                "msg_type,content,media_url,media_thumb_url,media_size,media_duration,"
                "media_format,expire_time) VALUES (?,?,?,?,?,?,?,?,?,?,?,NOW()+INTERVAL 7 DAY)",
                [](const drogon::orm::Result&) {},
                [](const drogon::orm::DrogonDbException& e) {
                    LOG_ERROR << "AsyncMessageWriter: " << e.base().what();
                },
                fromUserId, toUserId, chatType, capturedTargetId, msgType,
                content, mediaUrl, thumbUrl, mediaSize, mediaDuration, mediaFormat
            );
        });
    }

    std::thread worker_;
    std::atomic<bool> running_;
    redisContext* flusherRedis_;
    redisContext* workerRedis_;

    std::queue<std::string> msgQueue_;
    std::mutex queueMtx_;
    std::condition_variable queueCv_;
    std::thread flusher_;
    std::atomic<bool> flusherRunning_{false};
};
