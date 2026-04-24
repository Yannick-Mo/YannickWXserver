#include "FileUploadCtrl.h"
#include <drogon/utils/Utilities.h>
#include <drogon/HttpAppFramework.h>
#include <json/json.h>
#include <fstream>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;
using namespace drogon;

// ---------- 线程池 ----------
class FileWritePool {
public:
    static FileWritePool& instance(size_t poolSize = 0) {
        static FileWritePool pool(poolSize);
        return pool;
    }
    void enqueue(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            tasks_.push(std::move(task));
        }
        cond_.notify_one();
    }
private:
    FileWritePool(size_t poolSize) : stop_(false) {
        if (poolSize == 0) poolSize = std::thread::hardware_concurrency() * 2;
        for (size_t i = 0; i < poolSize; ++i)
            workers_.emplace_back([this] { work(); });
    }
    ~FileWritePool() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        cond_.notify_all();
        for (auto& t : workers_) if (t.joinable()) t.join();
    }
    void work() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cond_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool stop_;
};

// ---------- 辅助函数 ----------
static bool isSafePath(const fs::path& base, const fs::path& sub) {
    try {
        auto resolvedBase = fs::weakly_canonical(base);
        auto resolvedSub = fs::weakly_canonical(sub);
        auto rel = resolvedSub.lexically_relative(resolvedBase);
        return !rel.empty() && rel.native()[0] != '.';
    } catch (...) { return false; }
}

static std::string sanitizeExtensionHelper(const std::string& ext, const std::unordered_set<std::string>& whitelist) {
    if (ext.empty()) return "";
    std::string clean;
    for (char c : ext) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.')
            clean += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    auto pos = clean.find_last_of('.');
    std::string suffix = (pos != std::string::npos) ? clean.substr(pos) : "." + clean;
    if (whitelist.find(suffix) != whitelist.end()) return suffix;
    return "";
}


namespace api::v1 {
// ---------- 构造函数 ----------
FileUploadCtrl::FileUploadCtrl() {
    auto& cfg = app().getCustomConfig();
    auto& uploadCfg = cfg["upload"];

    finalDir_ = uploadCfg.get("upload_dir", "./uploads/files/").asString();
    if (!finalDir_.empty() && finalDir_.back() != '/') finalDir_.push_back('/');
    chunkBaseDir_ = finalDir_ + "chunks/";

    maxChunkSize_ = uploadCfg.get("max_chunk_size", 10 * 1024 * 1024).asUInt64();
    maxRetries_ = uploadCfg.get("max_retries", 5).asInt();

    if (uploadCfg.isMember("allowed_extensions") && uploadCfg["allowed_extensions"].isArray()) {
        for (auto& ext : uploadCfg["allowed_extensions"]) {
            std::string e = ext.asString();
            std::transform(e.begin(), e.end(), e.begin(), ::tolower);
            if (!e.empty() && e[0] != '.') e = "." + e;
            allowedExtensions_.insert(e);
        }
    } else {
        allowedExtensions_ = {".jpg",".jpeg",".png",".gif",".webp",".mp4",".mov",".pdf",".txt"};
    }

    ensureDir(finalDir_);
    ensureDir(chunkBaseDir_);
    size_t poolSize = uploadCfg.get("thread_pool_size", 0).asUInt();
    FileWritePool::instance(poolSize);
}

bool FileUploadCtrl::ensureDir(const std::string& dir) const {
    try {
        if (!fs::exists(dir)) return fs::create_directories(dir);
        return true;
    } catch (...) { return false; }
}

std::string FileUploadCtrl::getChunkDir(const std::string& fileHash) const {
    return chunkBaseDir_ + fileHash + "/";
}

std::string FileUploadCtrl::getChunkPath(const std::string& fileHash, int chunkIndex) const {
    return getChunkDir(fileHash) + "chunk_" + std::to_string(chunkIndex);
}

bool FileUploadCtrl::isAllowedExtension(const std::string& ext) const {
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return allowedExtensions_.find(lower) != allowedExtensions_.end();
}

std::string FileUploadCtrl::sanitizeExtension(const std::string& ext) const {
    return sanitizeExtensionHelper(ext, allowedExtensions_);
}

std::string FileUploadCtrl::generateUniqueFilename(const std::string& ext, const std::string& dir) const {
    for (int i = 0; i < maxRetries_; ++i) {
        std::string name = utils::getUuid() + ext;
        if (!fs::exists(fs::path(dir) / name)) return name;
        std::this_thread::sleep_for(std::chrono::milliseconds(1 << i));
    }
    throw std::runtime_error("Failed to generate unique filename");
}

void FileUploadCtrl::saveChunkAsync(const std::string& path, std::shared_ptr<std::vector<char>> data,
                                    std::function<void(bool)>&& callback) const {
    FileWritePool::instance().enqueue([path, data, callback]() {
        bool ok = false;
        {
            std::ofstream out(path, std::ios::binary);
            if (out) {
                out.write(data->data(), data->size());
                ok = out.good();
            }
        }
        app().getLoop()->queueInLoop([callback, ok]() { callback(ok); });
    });
}

void FileUploadCtrl::mergeChunks(const std::string& fileHash, int totalChunks,
                                 const std::string& finalFilename,
                                 std::function<void(bool, const std::string&)>&& callback) const {
    FileWritePool::instance().enqueue([=]() {
        std::string finalPath = finalDir_ + finalFilename;
        bool success = false;
        std::string errorMsg;
        std::ofstream out(finalPath, std::ios::binary);
        if (!out) {
            errorMsg = "Cannot create final file";
            app().getLoop()->queueInLoop([callback, success, errorMsg]() { callback(success, errorMsg); });
            return;
        }
        std::string chunkDir = getChunkDir(fileHash);
        for (int i = 0; i < totalChunks; ++i) {
            std::string chunkPath = chunkDir + "chunk_" + std::to_string(i);
            std::ifstream in(chunkPath, std::ios::binary);
            if (!in) {
                errorMsg = "Missing chunk " + std::to_string(i);
                out.close();
                fs::remove(finalPath);
                app().getLoop()->queueInLoop([callback, success, errorMsg]() { callback(success, errorMsg); });
                return;
            }
            out << in.rdbuf();
            in.close();
        }
        out.close();
        success = true;
        fs::remove_all(chunkDir);
        app().getLoop()->queueInLoop([callback, success, errorMsg]() { callback(success, errorMsg); });
    });
}

void FileUploadCtrl::saveThumbnailAsync(const std::string& path, std::shared_ptr<std::vector<char>> data,
                                        std::function<void(bool)>&& callback) const {
    FileWritePool::instance().enqueue([path, data, callback]() {
        bool ok = false;
        {
            std::ofstream out(path, std::ios::binary);
            if (out) {
                out.write(data->data(), data->size());
                ok = out.good();
            }
        }
        app().getLoop()->queueInLoop([callback, ok]() { callback(ok); });
    });
}

// ---------- 主请求路由 ----------
void FileUploadCtrl::asyncHandleHttpRequest(const HttpRequestPtr& req,
                                             std::function<void(const HttpResponsePtr&)>&& callback) {
    const std::string& path = req->getPath();
    const auto method = req->getMethod();

    if (path == "/api/v1/upload/chunk") {
        if (method == Get) {
            // ---------- 查询分片状态 ----------
            auto params = req->getParameters();
            auto hashIt = params.find("hash");
            auto totalIt = params.find("total_chunks");
            if (hashIt == params.end() || totalIt == params.end()) {
                Json::Value resp;
                resp["error"] = "Missing hash or total_chunks";
                auto httpResp = HttpResponse::newHttpJsonResponse(resp);
                httpResp->setStatusCode(k400BadRequest);
                callback(httpResp);
                return;
            }
            std::string hash = hashIt->second;
            int total = std::stoi(totalIt->second);
            std::string chunkDir = getChunkDir(hash);
            Json::Value uploaded(Json::arrayValue);
            if (fs::exists(chunkDir)) {
                for (int i = 0; i < total; ++i) {
                    if (fs::exists(chunkDir + "chunk_" + std::to_string(i)))
                        uploaded.append(i);
                }
            }
            Json::Value resp;
            resp["uploaded_chunks"] = uploaded;
            callback(HttpResponse::newHttpJsonResponse(resp));
            return;
        } 
        else if (method == Post) {
            // ---------- 上传分片 ----------
            MultiPartParser parser;
            if (parser.parse(req) != 0) {
                Json::Value resp; resp["error"] = "Invalid multipart";
                auto httpResp = HttpResponse::newHttpJsonResponse(resp);
                httpResp->setStatusCode(k400BadRequest);
                callback(httpResp);
                return;
            }

            auto& params = parser.getParameters();
            auto hashIt = params.find("hash");
            auto indexIt = params.find("index");
            if (hashIt == params.end() || indexIt == params.end()) {
                Json::Value resp; resp["error"] = "Missing hash or index field";
                auto httpResp = HttpResponse::newHttpJsonResponse(resp);
                httpResp->setStatusCode(k400BadRequest);
                callback(httpResp);
                return;
            }
            std::string fileHash = hashIt->second;
            int chunkIndex = std::stoi(indexIt->second);

            const HttpFile* chunkFile = nullptr;
            const auto& files = parser.getFiles();
            for (const auto& f : files) {
                if (f.getItemName() == "chunk") {
                    chunkFile = &f;
                    break;
                }
            }
            if (!chunkFile || chunkFile->fileLength() == 0) {
                Json::Value resp; resp["error"] = "Missing chunk file";
                auto httpResp = HttpResponse::newHttpJsonResponse(resp);
                httpResp->setStatusCode(k400BadRequest);
                callback(httpResp);
                return;
            }

            if (chunkFile->fileLength() > maxChunkSize_) {
                Json::Value resp; resp["error"] = "Chunk too large";
                auto httpResp = HttpResponse::newHttpJsonResponse(resp);
                httpResp->setStatusCode(k413RequestEntityTooLarge);
                callback(httpResp);
                return;
            }

            std::string chunkDir = getChunkDir(fileHash);
            if (!ensureDir(chunkDir)) {
                Json::Value resp; resp["error"] = "Cannot create chunk dir";
                auto httpResp = HttpResponse::newHttpJsonResponse(resp);
                httpResp->setStatusCode(k500InternalServerError);
                callback(httpResp);
                return;
            }

            std::string chunkPath = getChunkPath(fileHash, chunkIndex);
            auto data = std::make_shared<std::vector<char>>(
                chunkFile->fileData(),
                chunkFile->fileData() + chunkFile->fileLength()
            );
            saveChunkAsync(chunkPath, data, [callback](bool ok) {
                Json::Value resp;
                if (ok) {
                    resp["success"] = true;
                    callback(HttpResponse::newHttpJsonResponse(resp));
                } else {
                    resp["error"] = "Failed to save chunk";
                    auto httpResp = HttpResponse::newHttpJsonResponse(resp);
                    httpResp->setStatusCode(k500InternalServerError);
                    callback(httpResp);
                }
            });
            return;
        }
        else {
            // 不支持的 HTTP 方法
            callback(HttpResponse::newNotFoundResponse());
            return;
        }
    }

    if (path == "/api/v1/upload/complete" && method == Post) {
        auto json = req->getJsonObject();
        if (!json || !json->isMember("hash") || !json->isMember("total_chunks") || !json->isMember("filename")) {
            Json::Value resp; resp["error"] = "Missing hash/total_chunks/filename";
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k400BadRequest);
            callback(httpResp);
            return;
        }
        std::string fileHash = (*json)["hash"].asString();
        int totalChunks = (*json)["total_chunks"].asInt();
        std::string originalName = (*json)["filename"].asString();
        std::string ext = fs::path(originalName).extension().string();
        std::string finalName;
        for (int i = 0; i < maxRetries_; ++i) {
            finalName = utils::getUuid() + ext;
            if (!fs::exists(finalDir_ + finalName)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1 << i));
        }
        if (finalName.empty()) {
            Json::Value resp; resp["error"] = "Failed to generate filename";
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k500InternalServerError);
            callback(httpResp);
            return;
        }
        mergeChunks(fileHash, totalChunks, finalName, [callback, finalName](bool success, const std::string& err) {
            Json::Value resp;
            if (success) {
                resp["success"] = true;
                resp["url"] = "/uploads/files/" + finalName;
                callback(HttpResponse::newHttpJsonResponse(resp));
            } else {
                resp["error"] = err;
                auto httpResp = HttpResponse::newHttpJsonResponse(resp);
                httpResp->setStatusCode(k500InternalServerError);
                callback(httpResp);
            }
        });
        return;
    }

    if (path == "/api/v1/upload/thumbnail" && method == Post) {
        MultiPartParser parser;
        if (parser.parse(req) != 0) {
            Json::Value resp; resp["error"] = "Invalid multipart";
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k400BadRequest);
            callback(httpResp);
            return;
        }

        const HttpFile* thumbFile = nullptr;
        const auto& files = parser.getFiles();
        for (const auto& f : files) {
            if (f.getItemName() == "thumbnail") {
                thumbFile = &f;
                break;
            }
        }
        if (!thumbFile || thumbFile->fileLength() == 0) {
            Json::Value resp; resp["error"] = "Missing thumbnail file";
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k400BadRequest);
            callback(httpResp);
            return;
        }

        std::string_view extRaw = thumbFile->getFileExtension();
        std::string ext(extRaw);
        ext = sanitizeExtension(ext);
        if (ext.empty()) {
            Json::Value resp; resp["error"] = "Unsupported thumbnail extension";
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k415UnsupportedMediaType);
            callback(httpResp);
            return;
        }

        if (!ensureDir(finalDir_)) {
            Json::Value resp; resp["error"] = "Cannot create final dir";
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k500InternalServerError);
            callback(httpResp);
            return;
        }

        std::string thumbName;
        for (int i = 0; i < maxRetries_; ++i) {
            thumbName = utils::getUuid() + ext;
            if (!fs::exists(finalDir_ + thumbName)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1 << i));
        }
        if (thumbName.empty()) {
            Json::Value resp; resp["error"] = "Failed to generate thumbnail name";
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k500InternalServerError);
            callback(httpResp);
            return;
        }

        std::string thumbPath = finalDir_ + thumbName;
        auto data = std::make_shared<std::vector<char>>(
            thumbFile->fileData(),
            thumbFile->fileData() + thumbFile->fileLength()
        );
        saveThumbnailAsync(thumbPath, data, [callback, thumbName](bool ok) {
            Json::Value resp;
            if (ok) {
                resp["url"] = "/uploads/files/" + thumbName;
                callback(HttpResponse::newHttpJsonResponse(resp));
            } else {
                resp["error"] = "Failed to save thumbnail";
                auto httpResp = HttpResponse::newHttpJsonResponse(resp);
                httpResp->setStatusCode(k500InternalServerError);
                callback(httpResp);
            }
        });
        return;
    }

    // 未匹配任何有效路径
    callback(HttpResponse::newNotFoundResponse());
}



}