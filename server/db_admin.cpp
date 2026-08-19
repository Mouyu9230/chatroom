#include "db_admin.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/select.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../database/db.hpp"
#include "../database/db_pool.hpp"

namespace dbadmin {
namespace {

using Row  = std::vector<std::string>;
using Rows = std::vector<Row>;

// 单元格最大显示宽度, 过长截断并用 '~' 标记
constexpr size_t kMaxCol = 48;

std::string clip(const std::string& s, size_t w) {
    if (s.size() <= w) return s;
    return s.substr(0, w - 1) + "~";
}

// 执行 SELECT, 收集全部结果行; 失败返回 false, db.error() 给出原因。
bool collect(Db& db, const std::string& sql, Rows& rows) {
    rows.clear();
    return db.query(sql, [&](const Row& r) { rows.push_back(r); return true; });
}

// 带表头的对齐打印: 先收集再统一算列宽。
void print_table(const std::string& title, const std::vector<std::string>& headers,
                 const Rows& rows) {
    printf("\n== %s ==\n", title.c_str());
    if (rows.empty()) {
        printf("(empty)\n");
        return;
    }
    std::vector<size_t> width(headers.size());
    for (size_t c = 0; c < headers.size(); ++c) width[c] = headers[c].size();
    for (const auto& row : rows) {
        for (size_t c = 0; c < row.size() && c < width.size(); ++c) {
            size_t len = row[c].size() > kMaxCol ? kMaxCol : row[c].size();
            if (len > width[c]) width[c] = len;
        }
    }
    for (size_t c = 0; c < headers.size(); ++c)
        printf("%-*s  ", static_cast<int>(width[c]), headers[c].c_str());
    printf("\n");
    for (size_t c = 0; c < headers.size(); ++c)
        printf("%-*s  ", static_cast<int>(width[c]), std::string(width[c], '-').c_str());
    printf("\n");
    for (const auto& row : rows) {
        for (size_t c = 0; c < headers.size(); ++c) {
            std::string cell = (c < row.size()) ? row[c] : std::string();
            printf("%-*s  ", static_cast<int>(width[c]), clip(cell, width[c]).c_str());
        }
        printf("\n");
    }
}

void print_help() {
    printf(
        "\nDB admin console — read-only view of the MySQL state.\n"
        "  help                   this help\n"
        "  users                  list all users\n"
        "  online                 list online users\n"
        "  friends [uid]          friend relations (all rows, or one user's list)\n"
        "  pending <uid>          pending friend requests for a user\n"
        "  blocks                 block relations\n"
        "  messages [n]           recent messages (default 20)\n"
        "  counts                 row counts per table\n"
        "  quit / exit            close console\n"
        "friends.status: 0=pending  1=accepted\n");
}

void run_command(const std::string& line) {
    // 按空白拆分命令与参数
    std::vector<std::string> tok;
    std::string cur;
    for (char ch : line) {
        if (ch == ' ' || ch == '\t') {
            if (!cur.empty()) { tok.push_back(cur); cur.clear(); }
        } else {
            cur += ch;
        }
    }
    if (!cur.empty()) tok.push_back(cur);
    if (tok.empty()) return;
    const std::string& cmd = tok[0];

    if (cmd == "help" || cmd == "h") { print_help(); return; }

    DbGuard g(db_pool());
    if (!g) {
        printf("[db] no connection available\n");
        return;
    }
    Db& db = *g;

    Rows rows;

    if (cmd == "users") {
        if (!collect(db, "SELECT user_id, username, nickname, online, created_at"
                         " FROM users ORDER BY user_id", rows)) {
            printf("[error] %s\n", db.error().c_str()); return;
        }
        print_table("users", {"user_id","username","nickname","online","created_at"}, rows);
        return;
    }
    if (cmd == "online") {
        if (!collect(db, "SELECT user_id, username, nickname FROM users"
                         " WHERE online=1 ORDER BY user_id", rows)) {
            printf("[error] %s\n", db.error().c_str()); return;
        }
        print_table("online", {"user_id","username","nickname"}, rows);
        return;
    }
    if (cmd == "friends") {
        if (tok.size() >= 2) {
            uint32_t uid = static_cast<uint32_t>(std::atoi(tok[1].c_str()));
            std::string sql = "SELECT f.friend_id, u.nickname, f.status, f.ts"
                              " FROM friends f JOIN users u ON u.user_id=f.friend_id"
                              " WHERE f.user_id=" + std::to_string(uid) +
                              " ORDER BY f.friend_id";
            if (!collect(db, sql, rows)) { printf("[error] %s\n", db.error().c_str()); return; }
            print_table("friends of " + std::to_string(uid),
                        {"friend_id","nickname","status","ts"}, rows);
        } else {
            if (!collect(db, "SELECT user_id, friend_id, status, remark, ts"
                             " FROM friends ORDER BY user_id, friend_id", rows)) {
                printf("[error] %s\n", db.error().c_str()); return;
            }
            print_table("friends (all)", {"user_id","friend_id","status","remark","ts"}, rows);
        }
        return;
    }
    if (cmd == "pending") {
        if (tok.size() < 2) { printf("usage: pending <uid>\n"); return; }
        uint32_t uid = static_cast<uint32_t>(std::atoi(tok[1].c_str()));
        std::string sql = "SELECT f.friend_id, u.nickname, f.remark, f.ts"
                          " FROM friends f JOIN users u ON u.user_id=f.friend_id"
                          " WHERE f.friend_id=" + std::to_string(uid) + " AND f.status=0"
                          " ORDER BY f.ts";
        if (!collect(db, sql, rows)) { printf("[error] %s\n", db.error().c_str()); return; }
        print_table("pending for " + std::to_string(uid),
                    {"friend_id","nickname","remark","ts"}, rows);
        return;
    }
    if (cmd == "blocks") {
        if (!collect(db, "SELECT blocker_id, blockee_id, ts FROM blocks"
                         " ORDER BY blocker_id, blockee_id", rows)) {
            printf("[error] %s\n", db.error().c_str()); return;
        }
        print_table("blocks", {"blocker_id","blockee_id","ts"}, rows);
        return;
    }
    if (cmd == "messages") {
        int n = 20;
        if (tok.size() >= 2) n = std::atoi(tok[1].c_str());
        if (n <= 0) n = 1;
        if (n > 200) n = 200;
        std::string sql = "SELECT msg_id, from_id, to_id, to_type, content, ts"
                          " FROM messages ORDER BY msg_id DESC LIMIT " + std::to_string(n);
        if (!collect(db, sql, rows)) { printf("[error] %s\n", db.error().c_str()); return; }
        print_table("messages (latest " + std::to_string(n) + ")",
                    {"msg_id","from_id","to_id","to_type","content","ts"}, rows);
        return;
    }
    if (cmd == "counts") {
        if (!collect(db, "SELECT (SELECT COUNT(*) FROM users),"
                         " (SELECT COUNT(*) FROM friends),"
                         " (SELECT COUNT(*) FROM blocks),"
                         " (SELECT COUNT(*) FROM messages)", rows)) {
            printf("[error] %s\n", db.error().c_str()); return;
        }
        print_table("row counts", {"users","friends","blocks","messages"}, rows);
        return;
    }

    printf("unknown command: %s  (type 'help')\n", cmd.c_str());
}

void console_loop() {
    fprintf(stdout, "[admin] DB console ready. type 'help' for commands.\n");
    std::string line;
    while (g_running) {
        // 非阻塞探测 stdin: 有输入才 getline, 否则定期醒来检查服务端是否停机,
        // 避免无限阻塞在 getline 上(停机时 glibc 清理 stdin 流会与 getline 持锁死锁)。
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = {0, 200000};   // 200ms
        int r = select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;  // 信号打断, 回循环重查停机标志
            break;                          // 输入源异常(如 fd 被关闭), 结束控制台
        }
        if (r == 0) continue;               // 超时无输入, 回循环检查 g_running
        if (!FD_ISSET(STDIN_FILENO, &rfds)) continue;

        if (!std::getline(std::cin, line)) break;  // EOF / 读错误
        // 去除首尾空白
        std::string t = line;
        size_t b = t.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        size_t e = t.find_last_not_of(" \t");
        t = t.substr(b, e - b + 1);
        if (t == "quit" || t == "exit") break;
        run_command(t);
    }
    fprintf(stdout, "[admin] DB console closed.\n");
}

}  // namespace

void start_console() {
    std::thread(console_loop).detach();
}

}  // namespace dbadmin
