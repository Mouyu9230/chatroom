#pragma once

#include <functional>
#include <string>
#include <vector>

#include "db_config.hpp"

// ============================================================
//  Db —— 单个 MySQL 连接的最小 RAII 封装
//
//  职责: 连接/断开、SQL 执行与查询、字符串转义、错误信息。
//  线程安全: 非线程安全 —— 同一 Db 同一时刻只能被一个线程使用,
//  由 DbPool 保证: 每次只借出一个连接给一个工作线程。
//
//  用法示例:
//    Db db;
//    DbConfig cfg; cfg.load_from_env();
//    if (!db.connect(cfg)) { ... }
//    db.execute("UPDATE users SET online=1 WHERE user_id=1");
//    db.query("SELECT nickname FROM users WHERE user_id=1",
//             [](const std::vector<std::string>& row) { ... return true; });
// ============================================================
class Db {
public:
    Db() = default;
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    Db(Db&&) noexcept;
    Db& operator=(Db&&) noexcept;

    bool connect(const DbConfig& cfg);
    void close();

    bool ok() const { return mysql_ != nullptr; }
    const std::string& error() const { return err_; }

    // 对字符串做转义, 用于安全拼接 SQL(防止注入)。
    std::string escape(const std::string& s) const;

    // 执行 INSERT/UPDATE/DELETE 等无结果集语句。
    // 成功返回 true; affected/last_id 可选输出。
    bool execute(const std::string& sql,
                 unsigned long long* affected = nullptr,
                 unsigned long long* last_id = nullptr);

    // 执行 SELECT, 逐行回调; 回调返回 false 可提前终止。
    using RowCallback = std::function<bool(const std::vector<std::string>&)>;
    bool query(const std::string& sql, const RowCallback& cb);

private:
    // MYSQL* 以 void* 形式持有, 避免头文件依赖 mysql.h(见 db.cpp 内转换)
    void* mysql_ = nullptr;
    std::string err_;
};
