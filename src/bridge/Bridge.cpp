#include "Bridge.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace lxe {

namespace {
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;
} // namespace

void Bridge::ReplyError(const Json& id, bool hasId, int code, const std::string& message) {
    if (!hasId) return;
    Json resp = Json::object();
    resp["id"] = id;
    Json err = Json::object();
    err["code"] = code;
    err["message"] = message;
    resp["error"] = err;
    MaybePost(resp.dump());
}

void Bridge::HandleMessage(const std::string& jsonText) {
    Json req;
    try {
        req = Json::parse(jsonText);
    } catch (const JsonException& e) {
        ReplyError(Json::object(), true, kParseError, std::string("invalid JSON: ") + e.what());
        return;
    }

    if (!req.isObject()) {
        ReplyError(Json::object(), true, kInvalidRequest, "request must be a JSON object");
        return;
    }

    bool hasId = req.contains("id");
    Json id = hasId ? req.at("id") : Json::object();

    if (!req.contains("method") || !req.at("method").isString()) {
        ReplyError(id, hasId, kInvalidRequest, "missing string field 'method'");
        return;
    }
    const std::string method = req.at("method").asString();

    Json params = Json::object();
    if (req.contains("params")) {
        const Json& p = req.at("params");
        if (!p.isObject() && !p.isArray()) {
            ReplyError(id, hasId, kInvalidParams, "'params' must be an object or array");
            return;
        }
        params = p;
    }

    auto it = handlers_.find(method);
    if (it == handlers_.end()) {
        // 异步处理器：立即返回，后台线程完成后通过 done 回调投递响应
        auto ait = asyncHandlers_.find(method);
        if (ait == asyncHandlers_.end()) {
            ReplyError(id, hasId, kMethodNotFound, "unknown method: " + method);
            return;
        }
        try {
            ait->second(params, [this, id, hasId, method](HandlerResult result) {
                SendResult(id, hasId, result, method);
            });
        } catch (const std::exception& e) {
            HandlerResult r;
            r.ok = false;
            r.errCode = kInternalError;
            r.errMsg = std::string("async handler threw: ") + e.what();
            SendResult(id, hasId, r, method);
        }
        return;
    }

    HandlerResult result;
    try {
        result = it->second(params);
    } catch (const std::exception& e) {
        result.ok = false;
        result.errCode = kInternalError;
        result.errMsg = std::string("handler threw: ") + e.what();
    }

    SendResult(id, hasId, result, method);
}

void Bridge::SendResult(const Json& id, bool hasId, const HandlerResult& result, const std::string& method) {
    if (!hasId) {
        // 通知类消息：仍记录活动日志供前端诊断
        PostActivity(method, result.ok, result.errMsg);
        return;
    }

    Json resp = Json::object();
    resp["id"] = id;
    if (result.ok) {
        resp["result"] = result.result;
    } else {
        Json err = Json::object();
        err["code"] = result.errCode != 0 ? result.errCode : kInternalError;
        err["message"] = result.errMsg.empty() ? "internal error" : result.errMsg;
        resp["error"] = err;
    }
    MaybePost(resp.dump());

    // 记录活动日志供前端诊断（请求-响应类）
    PostActivity(method, result.ok, result.errMsg);
}

void Bridge::PostActivity(const std::string& method, bool ok, const std::string& errMsg) {
    // 写入活动日志供 app.exportLogs 使用（互斥锁保护）
    {
        std::lock_guard<std::mutex> lk(activityMu_);
        ActivityEntry e;
        e.method = method;
        e.ok = ok;
        e.error = errMsg;
        // 生成时间戳（HH:MM:SS.mmm）
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        std::tm tm{};
        localtime_s(&tm, &t);
        std::ostringstream ss;
        ss << std::put_time(&tm, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
        e.time = ss.str();
        activityLog_.push_back(std::move(e));
        if (activityLog_.size() > 2000) activityLog_.erase(activityLog_.begin());
    }
    if (closed_.load(std::memory_order_acquire) || !poster_) return;
    Json activity = Json::object();
    activity["method"] = method;
    activity["ok"] = ok;
    if (!errMsg.empty()) activity["error"] = errMsg;

    // 生成时间戳（HH:MM:SS.mmm）
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
    localtime_s(&tm, &t);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    activity["time"] = ss.str();

    PostEvent("bridge.activity", activity);
}

void Bridge::PostEvent(const std::string& eventName, const Json& data) {
    Json event = Json::object();
    event["event"] = eventName;
    event["data"] = data;
    MaybePost(event.dump());
}

} // namespace lxe