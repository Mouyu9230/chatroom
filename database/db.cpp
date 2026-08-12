#include "db.hpp"

#include <mysql/mysql.h>

#include <utility>

// 把内部 void* 转回 MYSQL*(MySQL 8 的 MYSQL 是 struct MYSQL 的 typedef)
static MYSQL* m(void* p) { return static_cast<MYSQL*>(p); }

Db::~Db() { close(); }

Db::Db(Db&& o) noexcept : mysql_(o.mysql_), err_(std::move(o.err_)) {
    o.mysql_ = nullptr;
}

Db& Db::operator=(Db&& o) noexcept {
    if (this != &o) {
        close();
        mysql_ = o.mysql_;
        err_   = std::move(o.err_);
        o.mysql_ = nullptr;
    }
    return *this;
}

bool Db::connect(const DbConfig& cfg) {
    close();
    mysql_ = mysql_init(nullptr);
    if (mysql_ == nullptr) {
        err_ = "mysql_init failed";
        return false;
    }

    unsigned int timeout = 3;
    mysql_options(m(mysql_), MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(m(mysql_), MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (mysql_real_connect(m(mysql_), cfg.host.c_str(), cfg.user.c_str(),
                           cfg.password.c_str(), cfg.database.c_str(),
                           cfg.port, nullptr, 0) == nullptr) {
        err_ = mysql_error(m(mysql_));
        mysql_close(m(mysql_));
        mysql_ = nullptr;
        return false;
    }
    err_.clear();
    return true;
}

void Db::close() {
    if (mysql_ != nullptr) {
        mysql_close(m(mysql_));
        mysql_ = nullptr;
    }
}

std::string Db::escape(const std::string& s) const {
    if (mysql_ == nullptr) return s;
    std::string out;
    out.resize(s.size() * 2 + 1);
    unsigned long n = mysql_real_escape_string(m(const_cast<void*>(mysql_)),
                                               out.data(), s.data(), s.size());
    out.resize(n);
    return out;
}

bool Db::execute(const std::string& sql,
                 unsigned long long* affected,
                 unsigned long long* last_id) {
    if (mysql_ == nullptr) {
        err_ = "not connected";
        return false;
    }
    if (mysql_real_query(m(mysql_), sql.data(), sql.size()) != 0) {
        err_ = mysql_error(m(mysql_));
        return false;
    }
    if (affected) *affected = mysql_affected_rows(m(mysql_));
    if (last_id)  *last_id   = mysql_insert_id(m(mysql_));
    err_.clear();
    return true;
}

bool Db::query(const std::string& sql, const RowCallback& cb) {
    if (mysql_ == nullptr) {
        err_ = "not connected";
        return false;
    }
    if (mysql_real_query(m(mysql_), sql.data(), sql.size()) != 0) {
        err_ = mysql_error(m(mysql_));
        return false;
    }
    MYSQL_RES* res = mysql_store_result(m(mysql_));
    if (res == nullptr) {
        // 无结果集: 若确实不是 SELECT, 视为成功; 否则为错误
        if (mysql_field_count(m(mysql_)) == 0) {
            err_.clear();
            return true;
        }
        err_ = mysql_error(m(mysql_));
        return false;
    }

    const unsigned int num = mysql_num_fields(res);
    MYSQL_ROW row;
    std::vector<std::string> fields(num);
    while ((row = mysql_fetch_row(res)) != nullptr) {
        unsigned long* lens = mysql_fetch_lengths(res);
        for (unsigned int i = 0; i < num; ++i) {
            fields[i] = (row[i] != nullptr) ? std::string(row[i], lens[i]) : std::string();
        }
        if (cb && !cb(fields)) break;
    }
    mysql_free_result(res);
    err_.clear();
    return true;
}
