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
#include "group_db.hpp"

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

    check(!db::user::friend_are_friends(db, alice_id, bob_id), "not friend before accept");

    // ---- accept by chat (bob sends chat to alice) ----
    bool accepted = db::user::friend_accept_by_chat(db, bob_id, alice_id);
    check(accepted, "bob->alice chat accepts alice's request");

    check(db::user::friend_are_friends(db, alice_id, bob_id), "now is friend");

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

    // ---- block + del: 删除好友应同时清除拉黑状态 ----
    r = db::user::friend_block(db, alice_id, bob_id, true);
    check(r == protocol::user::ERR_SUCCESS, "alice block bob");
    check(db::user::friend_is_blocked(db, bob_id, alice_id),
          "block visible in reverse direction");
    r = db::user::friend_del(db, alice_id, bob_id);
    check(r == protocol::user::ERR_SUCCESS &&
              !db::user::friend_is_blocked(db, alice_id, bob_id) &&
              !db::user::friend_is_blocked(db, bob_id, alice_id),
          "friend_del clears block -> chat would return ERR_NOT_FRIEND");

    // ---- group: create / join / approve / reject / promote / remove / dissolve ----
    uint32_t gid = 0;
    r = db::group::create_group(db, alice_id, "SmokeGroup", gid);
    check(r == protocol::user::ERR_SUCCESS && gid > 0, "create_group alice -> owner");
    uint32_t gid2 = 0;
    r = db::group::create_group(db, alice_id, "", gid2);
    check(r == protocol::user::ERR_INVALID_PARAM, "create_group empty name -> ERR_INVALID_PARAM");

    check(db::group::group_exists(db, gid), "group_exists");
    check(db::group::member_role(db, gid, alice_id) == protocol::group::GROUP_ROLE_OWNER,
          "alice is owner");
    r = db::group::join_request(db, alice_id, gid, "self");
    check(r == protocol::user::ERR_ALREADY_IN_GROUP, "owner join own group -> ERR_ALREADY_IN_GROUP");

    r = db::group::join_request(db, bob_id, gid, "hi");
    check(r == protocol::user::ERR_SUCCESS, "bob join_request");
    r = db::group::join_request(db, bob_id, gid, "again");
    check(r == protocol::user::ERR_REQUEST_PENDING, "bob duplicate join -> ERR_REQUEST_PENDING");

    uint32_t carol_id = 0;
    r = db::user::register_user(db, "smoke_carol", "carol123", "Carol", carol_id);
    check(r == protocol::user::ERR_SUCCESS && carol_id > 0, "register carol");

    std::vector<protocol::group::GroupPendingItem> gpending;
    r = db::group::pending_list(db, alice_id, gid, gpending);
    check(r == protocol::user::ERR_SUCCESS && gpending.size() == 1 &&
              gpending[0].user_id() == bob_id && gpending[0].nickname() == "Bob",
          "owner pending_list has bob");

    r = db::group::approve_join(db, alice_id, gid, bob_id);
    check(r == protocol::user::ERR_SUCCESS, "owner approve bob");
    check(db::group::member_role(db, gid, bob_id) == protocol::group::GROUP_ROLE_MEMBER,
          "bob now member");
    r = db::group::approve_join(db, alice_id, gid, bob_id);
    check(r == protocol::user::ERR_ALREADY_IN_GROUP, "double approve -> ERR_ALREADY_IN_GROUP");

    r = db::group::join_request(db, carol_id, gid, "me too");
    check(r == protocol::user::ERR_SUCCESS, "carol join_request");
    r = db::group::reject_join(db, alice_id, gid, carol_id);
    check(r == protocol::user::ERR_SUCCESS, "owner reject carol");

    r = db::group::promote_admin(db, alice_id, gid, bob_id);
    check(r == protocol::user::ERR_SUCCESS, "owner promote bob to admin");
    check(db::group::member_role(db, gid, bob_id) == protocol::group::GROUP_ROLE_ADMIN,
          "bob now admin");

    // 管理员仅能移除普通成员
    r = db::group::join_request(db, carol_id, gid, "try again");
    check(r == protocol::user::ERR_SUCCESS, "carol rejoin_request");
    r = db::group::approve_join(db, alice_id, gid, carol_id);
    check(r == protocol::user::ERR_SUCCESS, "owner approve carol");
    r = db::group::remove_member(db, bob_id, gid, carol_id);
    check(r == protocol::user::ERR_SUCCESS, "admin remove member carol");
    check(db::group::member_role(db, gid, carol_id) == 0, "carol removed");
    r = db::group::remove_member(db, bob_id, gid, alice_id);
    check(r == protocol::user::ERR_INVALID_PARAM, "admin cannot remove owner");
    r = db::group::remove_member(db, bob_id, gid, bob_id);
    check(r == protocol::user::ERR_INVALID_PARAM, "cannot remove self");

    // 成员列表 & 我的群
    std::vector<protocol::group::GroupMemberItem> members;
    r = db::group::member_list(db, bob_id, gid, members);
    check(r == protocol::user::ERR_SUCCESS && members.size() == 2, "member_list has 2");
    std::vector<protocol::group::GroupListItem> mine;
    r = db::group::my_groups(db, bob_id, mine);
    check(r == protocol::user::ERR_SUCCESS && mine.size() == 1 && mine[0].group_id() == gid,
          "my_groups bob has group");

    // 群消息落库 + 群历史 + 1:1 历史不串(防御 to_type 区分)
    uint64_t gmsg_id = 0;
    r = db::chat::save_message(db, bob_id, gid, protocol::chat::TARGET_TYPE_GROUP,
                               "hello group", gmsg_id, ts);
    check(r == protocol::user::ERR_SUCCESS && gmsg_id > 0, "save group message");
    std::vector<protocol::chat::ChatMessage> ghist;
    r = db::chat::query_group_history(db, gid, 0, 50, ghist);
    check(r == protocol::user::ERR_SUCCESS && ghist.size() == 1 &&
              ghist[0].content() == "hello group" &&
              ghist[0].to_type() == protocol::chat::TARGET_TYPE_GROUP,
          "query_group_history returns group message");
    history.clear();
    r = db::chat::query_history(db, alice_id, bob_id, 0, 50, history);
    check(r == protocol::user::ERR_SUCCESS &&
              (history.empty() || history[0].content() != "hello group"),
          "1:1 history does not leak group message");

    // ---- 群离线摘要: alice 所在群收到 bob 的 1 条群消息 ----
    std::vector<db::chat::OfflineGroupItem> goffline;
    r = db::chat::offline_group_summary(db, alice_id, 0, goffline);
    check(r == protocol::user::ERR_SUCCESS && goffline.size() == 1 &&
              goffline[0].group_id == gid &&
              goffline[0].group_name == "SmokeGroup" && goffline[0].count == 1,
          "offline_group_summary alice sees 1 group msg");

    // 自己发的群消息不计入自己
    goffline.clear();
    r = db::chat::offline_group_summary(db, bob_id, 0, goffline);
    check(r == protocol::user::ERR_SUCCESS && goffline.empty(),
          "offline_group_summary excludes self-sent msgs");

    // 解散
    r = db::group::dissolve_group(db, alice_id, gid);
    check(r == protocol::user::ERR_SUCCESS, "owner dissolve group");
    check(!db::group::group_exists(db, gid), "group gone after dissolve");
    r = db::group::pending_list(db, alice_id, gid, gpending);
    check(r == protocol::user::ERR_GROUP_NOT_FOUND, "pending after dissolve -> ERR_GROUP_NOT_FOUND");

    // ---- logout ----
    r = db::user::logout_user(db, alice_id);
    check(r == protocol::user::ERR_SUCCESS, "logout alice");

    // ---- cleanup test data ----
    db::user::friend_del(db, alice_id, bob_id);
    db.execute("DELETE FROM group_applications WHERE group_id=" + std::to_string(gid));
    db.execute("DELETE FROM group_members WHERE group_id=" + std::to_string(gid));
    db.execute("DELETE FROM group_info WHERE group_id=" + std::to_string(gid));
    db.execute("DELETE FROM messages WHERE from_id IN (" + std::to_string(alice_id) + "," +
               std::to_string(bob_id) + "," + std::to_string(carol_id) + ") OR to_id IN (" +
               std::to_string(alice_id) + "," + std::to_string(bob_id) + "," +
               std::to_string(carol_id) + ") OR (to_id=" + std::to_string(gid) +
               " AND to_type=2)");
    db.execute("DELETE FROM friends WHERE user_id IN (" + std::to_string(alice_id) + "," +
               std::to_string(bob_id) + "," + std::to_string(carol_id) + ") OR friend_id IN (" +
               std::to_string(alice_id) + "," + std::to_string(bob_id) + "," +
               std::to_string(carol_id) + ")");
    db.execute("DELETE FROM users WHERE user_id IN (" + std::to_string(alice_id) + "," +
               std::to_string(bob_id) + "," + std::to_string(carol_id) + ")");
    fprintf(stdout, "[db_smoke] cleanup done\n");

    fprintf(stdout, "\n[db_smoke] ===== %s (%d failed) =====\n",
            fail == 0 ? "ALL PASS" : "HAS FAILURES", fail);
    return fail == 0 ? 0 : 1;
}
