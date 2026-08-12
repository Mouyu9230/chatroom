#include "db_pool.hpp"

DbPool::~DbPool() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        closed_ = true;
    }
    cv_.notify_all();
    conns_.clear();  // 析构各连接
}

bool DbPool::init(const DbConfig& cfg) {
    std::size_t n = cfg.pool_size > 0 ? cfg.pool_size : 1;

    std::vector<std::unique_ptr<Db>> tmp;
    tmp.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        auto db = std::make_unique<Db>();
        if (!db->connect(cfg)) {
            return false;  // tmp 析构时自动关闭已建连接
        }
        tmp.push_back(std::move(db));
    }

    std::lock_guard<std::mutex> lock(mtx_);
    conns_ = std::move(tmp);
    size_  = n;
    closed_ = false;
    while (!free_.empty()) free_.pop();
    for (auto& db : conns_) free_.push(db.get());
    return true;
}

Db* DbPool::acquire() {
    std::unique_lock<std::mutex> lock(mtx_);
    if (conns_.empty()) return nullptr;  // 未 init 或已关闭: 快速失败, 避免阻塞
    cv_.wait(lock, [this] { return closed_ || !free_.empty(); });
    if (closed_) return nullptr;
    Db* db = free_.front();
    free_.pop();
    return db;
}

void DbPool::release(Db* db) {
    if (db == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        free_.push(db);
    }
    cv_.notify_one();
}

DbPool& db_pool() {
    static DbPool instance;
    return instance;
}
