#include "redis.hpp"

#include <sys/time.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include <hiredis/hiredis.h>

// ============================================================
//  Redis 封装实现 —— 基于 hiredis 同步 API
//
//  线程安全: 非线程安全, 单条连接同一时刻只能被一个线程使用,
//  由 RedisPool 保证借还(与 Db/DbPool 一致)。
//  断线自愈: 每条命令前检查连接错误, 出错则按 cfg_ 重连一次。
// ============================================================

void RedisConfig::load_from_env() {
    if (const char* v = std::getenv("CHATROOM_REDIS_HOST")) host = v;
    if (const char* v = std::getenv("CHATROOM_REDIS_PORT")) port = static_cast<uint16_t>(std::atoi(v));
    if (const char* v = std::getenv("CHATROOM_REDIS_PASS")) password = v;
    if (const char* v = std::getenv("CHATROOM_REDIS_POOL")) {
        int n = std::atoi(v);
        pool_size = (n > 0) ? static_cast<std::size_t>(n) : 1;
    }
}

namespace {
// void* 还原为 redisContext*。
redisContext* ctx_of(void* p) { return static_cast<redisContext*>(p); }
// 连接超时: 200ms, 避免 Redis 不可达时启动/取连接长期挂起。
struct timeval connect_timeout() {
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 200000;
    return tv;
}
}  // namespace

Redis::~Redis() { close(); }

Redis::Redis(Redis&& o) noexcept
    : ctx_(o.ctx_), cfg_(std::move(o.cfg_)), err_(std::move(o.err_)) {
    o.ctx_ = nullptr;
}

Redis& Redis::operator=(Redis&& o) noexcept {
    if (this != &o) {
        close();
        ctx_ = o.ctx_;
        cfg_ = std::move(o.cfg_);
        err_ = std::move(o.err_);
        o.ctx_ = nullptr;
    }
    return *this;
}

bool Redis::connect(const RedisConfig& cfg) {
    close();
    cfg_ = cfg;

    struct timeval tv = connect_timeout();
    redisContext* c = redisConnectWithTimeout(cfg.host.c_str(), cfg.port, tv);
    if (c == nullptr) {
        err_ = "redisConnectWithTimeout: out of memory";
        return false;
    }
    if (c->err != 0) {
        err_ = std::string("connect: ") + c->errstr;
        redisFree(c);
        return false;
    }
    if (!cfg.password.empty()) {
        redisReply* reply = static_cast<redisReply*>(redisCommand(c, "AUTH %s", cfg.password.c_str()));
        if (reply == nullptr) {
            err_ = std::string("AUTH: ") + c->errstr;
            redisFree(c);
            return false;
        }
        bool ok = reply->type == REDIS_REPLY_STATUS && std::strcmp(reply->str, "OK") == 0;
        freeReplyObject(reply);
        if (!ok) {
            err_ = "AUTH: wrong password";
            redisFree(c);
            return false;
        }
    }

    ctx_ = c;
    err_.clear();
    return true;
}

void Redis::close() {
    if (ctx_ != nullptr) {
        redisFree(ctx_of(ctx_));
        ctx_ = nullptr;
    }
}

bool Redis::reconnect() {
    close();
    return connect(cfg_);
}

bool Redis::ensure_connected() {
    if (ctx_ != nullptr && ctx_of(ctx_)->err == 0) return true;
    return reconnect();
}

bool Redis::get(const std::string& key, std::string& out, bool& found) {
    found = false;
    if (!ensure_connected()) return false;

    redisContext* c = ctx_of(ctx_);
    redisReply* reply = static_cast<redisReply*>(redisCommand(c, "GET %s", key.c_str()));
    if (reply == nullptr) {
        if (!reconnect()) return false;  // 断线自愈一次
        c = ctx_of(ctx_);
        reply = static_cast<redisReply*>(redisCommand(c, "GET %s", key.c_str()));
        if (reply == nullptr) {
            err_ = c->errstr ? c->errstr : "GET failed";
            return false;
        }
    }

    bool ok = true;
    if (reply->type == REDIS_REPLY_STRING) {
        out = reply->str;
        found = true;
    } else if (reply->type == REDIS_REPLY_NIL) {
        found = false;  // 键不存在: 视为未命中
    } else {
        err_ = std::string("GET unexpected reply type ") + std::to_string(reply->type);
        ok = false;
    }
    freeReplyObject(reply);
    return ok;
}

bool Redis::set(const std::string& key, const std::string& val, long ttl_sec) {
    if (!ensure_connected()) return false;

    redisContext* c = ctx_of(ctx_);
    redisReply* reply = static_cast<redisReply*>(
        ttl_sec > 0
            ? redisCommand(c, "SET %s %s EX %ld", key.c_str(), val.c_str(), ttl_sec)
            : redisCommand(c, "SET %s %s", key.c_str(), val.c_str()));
    if (reply == nullptr) {
        if (!reconnect()) return false;
        c = ctx_of(ctx_);
        reply = static_cast<redisReply*>(
            ttl_sec > 0
                ? redisCommand(c, "SET %s %s EX %ld", key.c_str(), val.c_str(), ttl_sec)
                : redisCommand(c, "SET %s %s", key.c_str(), val.c_str()));
        if (reply == nullptr) {
            err_ = c->errstr ? c->errstr : "SET failed";
            return false;
        }
    }

    bool ok = reply->type == REDIS_REPLY_STATUS && std::strcmp(reply->str, "OK") == 0;
    if (!ok) err_ = std::string("SET unexpected reply type ") + std::to_string(reply->type);
    freeReplyObject(reply);
    return ok;
}

bool Redis::del(const std::string& key) {
    if (!ensure_connected()) return false;

    redisContext* c = ctx_of(ctx_);
    redisReply* reply = static_cast<redisReply*>(redisCommand(c, "DEL %s", key.c_str()));
    if (reply == nullptr) {
        if (!reconnect()) return false;
        c = ctx_of(ctx_);
        reply = static_cast<redisReply*>(redisCommand(c, "DEL %s", key.c_str()));
        if (reply == nullptr) {
            err_ = c->errstr ? c->errstr : "DEL failed";
            return false;
        }
    }

    // DEL 返回被删除条数(REDIS_REPLY_INTEGER), 键不存在时返回 0 也算命令成功。
    bool ok = reply->type == REDIS_REPLY_INTEGER;
    if (!ok) err_ = std::string("DEL unexpected reply type ") + std::to_string(reply->type);
    freeReplyObject(reply);
    return ok;
}

bool Redis::ping() {
    if (!ensure_connected()) return false;

    redisContext* c = ctx_of(ctx_);
    redisReply* reply = static_cast<redisReply*>(redisCommand(c, "PING"));
    if (reply == nullptr) return false;
    bool ok = reply->type == REDIS_REPLY_STATUS && std::strcmp(reply->str, "PONG") == 0;
    freeReplyObject(reply);
    return ok;
}

// ============================================================
//  RedisPool
// ============================================================

RedisPool::~RedisPool() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        closed_ = true;
    }
    conns_.clear();  // 析构各连接
}

bool RedisPool::init(const RedisConfig& cfg) {
    std::size_t n = cfg.pool_size > 0 ? cfg.pool_size : 1;

    std::vector<std::unique_ptr<Redis>> tmp;
    tmp.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        auto r = std::make_unique<Redis>();
        if (!r->connect(cfg)) {
            // Redis 只是缓存: 不整体抛错, 保持池空, 由 acquire() 返回 nullptr 降级。
            return false;
        }
        tmp.push_back(std::move(r));
    }

    std::lock_guard<std::mutex> lock(mtx_);
    conns_  = std::move(tmp);
    size_   = n;
    closed_ = false;
    while (!free_.empty()) free_.pop();
    for (auto& r : conns_) free_.push(r.get());
    return true;
}

Redis* RedisPool::acquire() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (closed_ || free_.empty()) return nullptr;  // 非阻塞: 无空闲立即失败
    Redis* r = free_.front();
    free_.pop();
    return r;
}

void RedisPool::release(Redis* r) {
    if (r == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        free_.push(r);
    }
}

RedisPool& redis_pool() {
    static RedisPool instance;
    return instance;
}
