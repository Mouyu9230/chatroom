#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

// ============================================================
//  Redis 缓存封装 —— 与 MySQL(Db/DbPool)平行的最小实现
//
//  定位: 只为 database/user_db.cpp 的热读做 cache-aside 缓存,
//        MySQL 仍是唯一数据源(Source of Truth)。
//
//  核心约定 —— 优雅降级(fail-open):
//    - Redis 未启动/命令出错时, 一律回退到 MySQL, 不影响正确性;
//    - RedisPool::acquire() 非阻塞: 无空闲连接立即返回 nullptr(跳过缓存),
//      绝不因缓存繁忙阻塞主路径;
//    - 缓存写入失败忽略, 只影响命中率, 不影响业务。
// ============================================================

// ---- 连接配置: 环境变量 CHATROOM_REDIS_* ----
struct RedisConfig {
    std::string host      = "127.0.0.1";
    uint16_t    port      = 6379;
    std::string password  = "";        // 空 = 无密码
    std::size_t pool_size = 4;

    void load_from_env();
};

// ---- 单个连接的 RAII 封装(非线程安全, 由 RedisPool 分配使用) ----
class Redis {
public:
    Redis() = default;
    ~Redis();
    Redis(const Redis&) = delete;
    Redis& operator=(const Redis&) = delete;
    Redis(Redis&&) noexcept;
    Redis& operator=(Redis&&) noexcept;

    bool connect(const RedisConfig& cfg);
    void close();

    bool ok() const { return ctx_ != nullptr; }
    const std::string& error() const { return err_; }

    // GET: 命令成功返回 true, found 区分命中(有值)与 nil(无值)。
    //      命令出错返回 false(found 不保证), 调用方应回退 MySQL。
    bool get(const std::string& key, std::string& out, bool& found);
    // SET key val [EX ttl_sec]; ttl_sec <= 0 表示不过期。
    bool set(const std::string& key, const std::string& val, long ttl_sec = -1);
    // DEL key(用于缓存失效)。
    bool del(const std::string& key);
    // PING 探活。
    bool ping();

private:
    bool reconnect();        // 断线后按 cfg_ 重连一次
    bool ensure_connected(); // 命令前自愈: 断线则尝试重连
    void*  ctx_ = nullptr;   // redisContext*, 避免头文件依赖 hiredis
    RedisConfig cfg_;
    std::string err_;
};

// ---- 连接池(与 DbPool 同构; acquire 非阻塞, 见文件头说明) ----
class RedisPool {
public:
    RedisPool() = default;
    ~RedisPool();
    RedisPool(const RedisPool&) = delete;
    RedisPool& operator=(const RedisPool&) = delete;

    // 建立 cfg.pool_size 个连接; 任一失败整体失败(已建连接关闭),
    // 池保持空, 由 acquire() 返回 nullptr 优雅降级。
    bool init(const RedisConfig& cfg);

    Redis* acquire();        // 非阻塞: 未 init/已关闭/无空闲 → nullptr
    void release(Redis* r);  // 归还连接

    bool inited() const { return !conns_.empty(); }
    std::size_t size() const { return size_; }

private:
    std::size_t size_ = 0;
    std::vector<std::unique_ptr<Redis>> conns_;
    std::queue<Redis*> free_;
    std::mutex mtx_;
    bool closed_ = true;
};

// 全局单例, 供 database 层使用; server 启动时 init。
RedisPool& redis_pool();

// RAII 取/还连接: RedisGuard g(redis_pool()); if (!g) 回退 MySQL。
class RedisGuard {
public:
    explicit RedisGuard(RedisPool& pool) : pool_(pool), r_(pool.acquire()) {}
    ~RedisGuard() { if (r_) pool_.release(r_); }
    RedisGuard(const RedisGuard&) = delete;
    RedisGuard& operator=(const RedisGuard&) = delete;

    Redis* get() { return r_; }
    Redis* operator->() { return r_; }
    explicit operator bool() const { return r_ != nullptr; }

private:
    RedisPool& pool_;
    Redis* r_;
};
