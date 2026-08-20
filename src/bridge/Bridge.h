#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../Json.h"

namespace lxe {

class Bridge {
public:
    using PostFn = std::function<void(const std::string& utf8Json)>;

    struct HandlerResult {
        bool ok = true;
        Json result = Json::object();
        int errCode = 0;
        std::string errMsg;
    };
    using Handler = std::function<HandlerResult(const Json& params)>;
    // 异步处理器：立即返回，实际工作由注册方在后台线程完成后调用 done(result)。
    // done 可跨线程调用（内部通过 poster_ 队列/PostMessage 投递，线程安全）。
    using AsyncHandler = std::function<void(const Json& params, const std::function<void(HandlerResult)>& done)>;

    struct ActivityEntry {
        std::string time;
        std::string method;
        bool ok = true;
        std::string error;
    };

    void SetPoster(PostFn fn) { poster_ = std::move(fn); }
    bool Register(const std::string& method, Handler h) {
        if (handlers_.count(method)) return false;
        handlers_[method] = std::move(h);
        return true;
    }
    // 注册异步处理器（跑在后台线程，不阻塞 WebView2 消息线程 / 桥）。
    // 不能与同名的同步 Register 并存。
    bool RegisterAsync(const std::string& method, AsyncHandler h) {
        if (handlers_.count(method) || asyncHandlers_.count(method)) return false;
        asyncHandlers_[method] = std::move(h);
        return true;
    }

    // 关闭前调用：立即停止所有后续事件推送（跨线程安全）。
    // 防止后台线程（下载/监控）在 WebViewHost 析构后继续投递导致 use-after-free。
    void Close() { closed_.store(true, std::memory_order_release); }
    bool IsClosed() const { return closed_.load(std::memory_order_acquire); }

    void HandleMessage(const std::string& jsonText);
    void PostEvent(const std::string& eventName, const Json& data);
    // 供异步处理器在后台线程调用：投递请求的响应与活动日志（线程安全）
    void SendResult(const Json& id, bool hasId, const HandlerResult& result, const std::string& method);

    // 活动日志快照（互斥锁保护），用于 app.exportLogs RPC
    std::vector<ActivityEntry> SnapshotActivityLog() const {
        std::lock_guard<std::mutex> lk(activityMu_);
        return activityLog_;
    }

private:
    void MaybePost(const std::string& utf8Json) {
        if (!closed_.load(std::memory_order_acquire) && poster_) poster_(utf8Json);
    }
    void ReplyError(const Json& id, bool hasId, int code, const std::string& message);
    void PostActivity(const std::string& method, bool ok, const std::string& errMsg);

    PostFn poster_;
    std::map<std::string, Handler> handlers_;
    std::map<std::string, AsyncHandler> asyncHandlers_;
    mutable std::mutex activityMu_;
    std::vector<ActivityEntry> activityLog_;
    std::atomic<bool> closed_{false};
};

} // namespace lxe
