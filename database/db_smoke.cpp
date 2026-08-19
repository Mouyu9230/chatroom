// ============================================================
//  db_smoke —— MySQL 封装冒烟测试(独立可执行, 不依赖 handler)
//
//  用法: ./build/db_smoke        (默认连 127.0.0.1:3306 root 空密码 chatroom)
//        可通过 CHATROOM_DB_* 环境变量覆盖连接参数。
//
//  流程: 建表 → 注册 alice/bob → 重复注册 → 登录(对/错) →
//        加好友申请(重复申请) → 拉待处理列表 → 发私聊触发好友接受 →
//        好友检查 → 消息落库与历史拉取 → 登出 → 清理测试数据。
//  每步打印 PASS/FAIL, 退出码 0=全过。
// ============================================================

#include <cstdio>
#include <string>

#include "db_config.hpp"
#include "db_pool.hpp"
#include "db_init.hpp"
#include "user_db.hpp"
#include "chat_db.hpp"

int main() {
    DbConfig cfg;
    cfg.load_from_env();
    fprintf(stdout, "[db_smoke] connecting %s:%u db=%s user=%s pool=%zu\n",
            cfg.host.c_str(), cfg.port, cfg.database.c_str(), cfg.user.c_str(), cfg.pool_size);

    DbPool pool;
    if (!pool.init(cfg)) {
        fprintf(stderr, "[db_smoke] FAIL: cannot connect to MySQL (%s:%u db=%s).\n"
                        "  Did you create the database & account? e.g.\n"
                        "  sudo mysql -uroot -e \"CREATE DATABASE IF NOT EXISTS chatroom "
                        "CHARACTER SET utf8mb4; CREATE USER IF NOT EXISTS 'chatroom'@'127.0.0.1' "
                        "IDENTIFIED BY 'chatroom'; GRANT ALL ON chatroom.* TO 'chatroom'@'127.0.0.1';\"\n",
                cfg.host.c_str(), cfg.port, cfg.database.c_str());
        return 1;
    }
    DbGuard g(pool);
    Db& db = *g.get();

    std::string derr;
    if (!init_db(db, &derr)) {
        fprintf(stderr, "[db_smoke] FAIL: init_db error: %s\n", derr.c_str());
        return 1;
    }
    fprintf(stdout, "[db_smoke] init_db ok\n");

    int fail = 0;
    auto check = [&](bool ok, const char* name) {
        fprintf(stdout, "  %-42s %s\n", name, ok ? "PASS" : "FAIL");
        if (!ok) ++fail;
    };

    uint32_t alice_id = 0, bob_id = 0;

    // ---- register ----
    int r = db::user::register_user(db, "smoke_alice", "alice123", "Alice", alice_id);
    check(r == protocol::user::ERR_SUCCESS && alice_id > 0, "register alice");

    r = db::user::register_user(db, "smoke_bob", "bob123", "Bob", bob_id);
    check(r == protocol::user::ERR_SUCCESS && bob_id > 0, "register bob");

    uint32_t dup_id = 0;
    r = db::user::register_user(db, "smoke_alice", "x", "X", dup_id);
    check(r == protocol::user::ERR_USER_EXISTS, "register duplicate -> ERR_USER_EXISTS");

    // ---- login ----
    protocol::user::UserInfo info;
    uint64_t watermark = 0;
    r = db::user::login_user(db, "smoke_alice", "alice123", info, watermark);
    check(r == protocol::user::ERR_SUCCESS && info.nickname() == "Alice" && info.online(),
          "login alice ok");

    r = db::user::login_user(db, "smoke_alice", "wrong", info, watermark);
    check(r == protocol::user::ERR_INVALID_USER, "login wrong password -> ERR_INVALID_USER");

    // ---- friend request ----
    r = db::user::friend_request(db, alice_id, bob_id, "hi bob");
    check(r == protocol::user::ERR_SUCCESS, "friend_request alice->bob");

    r = db::user::friend_request(db, alice_id, bob_id, "again");
    check(r == protocol::user::ERR_REQUEST_PENDING, "duplicate request -> ERR_REQUEST_PENDING");

    std::vector<protocol::user::FriendPendingItem> pending;
    r = db::user::friend_pending_list(db, bob_id, pending);
    check(r == protocol::user::ERR_SUCCESS && pending.size() == 1 &&
              pending[0].friend_id() == alice_id && pending[0].nickname() == "Alice",
          "bob pending list has alice");

    bool is_friend = true;
    std::string nick;
    r = db::user::friend_check(db, alice_id, bob_id, is_friend, nick);
    check(r == protocol::user::ERR_SUCCESS && !is_friend, "not friend before accept");

    // ---- accept by chat (bob sends chat to alice) ----
    bool accepted = db::user::friend_accept_by_chat(db, bob_id, alice_id);
    check(accepted, "bob->alice chat accepts alice's request");

    r = db::user::friend_check(db, alice_id, bob_id, is_friend, nick);
    check(r == protocol::user::ERR_SUCCESS && is_friend && nick == "Bob", "now is friend");

    // ---- chat persistence ----
    uint64_t msg_id = 0, ts = 0;
    r = db::chat::save_message(db, alice_id, bob_id, 1, "hello bob", msg_id, ts);
    check(r == protocol::user::ERR_SUCCESS && msg_id > 0 && ts > 0, "save_message ok");

    std::vector<protocol::chat::ChatMessage> history;
    r = db::chat::query_history(db, alice_id, bob_id, 0, 50, history);
    check(r == protocol::user::ERR_SUCCESS && history.size() == 1 &&
              history[0].content() == "hello bob" && history[0].from_id() == alice_id,
          "query_history returns the message");

    // ---- offline summary (bob 未登录过, 水位为默认 0) ----
    std::vector<db::chat::OfflineItem> offline;
    r = db::chat::offline_summary(db, bob_id, 0, offline);
    check(r == protocol::user::ERR_SUCCESS && offline.size() == 1 &&
              offline[0].from_id == alice_id && offline[0].nickname == "Alice" &&
              offline[0].count == 1,
          "offline_summary groups bob's offline message by sender");

    // ---- logout ----
    r = db::user::logout_user(db, alice_id);
    check(r == protocol::user::ERR_SUCCESS, "logout alice");

    // ---- cleanup test data ----
    db::user::friend_del(db, alice_id, bob_id);
    db.execute("DELETE FROM messages WHERE from_id IN (" + std::to_string(alice_id) + "," +
               std::to_string(bob_id) + ") OR to_id IN (" + std::to_string(alice_id) + "," +
               std::to_string(bob_id) + ")");
    db.execute("DELETE FROM friends WHERE user_id IN (" + std::to_string(alice_id) + "," +
               std::to_string(bob_id) + ") OR friend_id IN (" + std::to_string(alice_id) + "," +
               std::to_string(bob_id) + ")");
    db.execute("DELETE FROM users WHERE user_id IN (" + std::to_string(alice_id) + "," +
               std::to_string(bob_id) + ")");
    fprintf(stdout, "[db_smoke] cleanup done\n");

    fprintf(stdout, "\n[db_smoke] ===== %s (%d failed) =====\n",
            fail == 0 ? "ALL PASS" : "HAS FAILURES", fail);
    return fail == 0 ? 0 : 1;
}
