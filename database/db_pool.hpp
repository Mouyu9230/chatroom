#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "db_config.hpp"
#include "db.hpp"

// ============================================================
//  DbPool —— 定长 MySQL 连接池
//
//  线程池的工作线程并发处理任务、都需要访问 DB; 而单个 Db 同一
//  时刻只能被一个线程使用, 因此用池做分配:
//    acquire() 无空闲连接时阻塞等待, release() 归还并唤醒。
//  池的 size 由 DbConfig::pool_size 决定(init 时生效)。
// ============================================================
class DbPool {
public:
    DbPool() = default;
    ~DbPool();
    DbPool(const DbPool&) = delete;
    DbPool& operator=(const DbPool&) = delete;

    // 建立 cfg.pool_size 个连接; 任一连接失败则整体失败(已建连接关闭)。
    bool init(const DbConfig& cfg);

    Db* acquire();        // 阻塞获取一个空闲连接; 池已关闭返回 nullptr
    void release(Db* db); // 归还连接

    bool   inited() const { return !conns_.empty(); }
    std::size_t size() const { return size_; }

private:
    std::size_t size_ = 0;
    std::vector<std::unique_ptr<Db>> conns_;
    std::queue<Db*> free_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool closed_ = false;
};

// 全局单例, 供 handler 等模块使用; server 启动时 init。
DbPool& db_pool();

// RAII 取/还连接: DbGuard g(db_pool()); g->login(...);
class DbGuard {
public:
    explicit DbGuard(DbPool& pool) : pool_(pool), db_(pool.acquire()) {}
    ~DbGuard() { if (db_) pool_.release(db_); }
    DbGuard(const DbGuard&) = delete;
    DbGuard& operator=(const DbGuard&) = delete;

    Db* get() { return db_; }
    Db* operator->() { return db_; }
    Db& operator*() { return *db_; }
    explicit operator bool() const { return db_ != nullptr; }

private:
    DbPool& pool_;
    Db* db_;
};
