#include "user_db.hpp"

#include <chrono>
#include <cstdlib>

namespace db {
namespace user {

namespace {

uint64_t now_sec() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// 查询 a→b 方向的 relation status: 返回 -2 表示无记录, 否则返回 status 值。
int rel_status(Db& db, uint32_t a, uint32_t b) {
    std::string sql = "SELECT status FROM friends WHERE user_id=" + std::to_string(a) +
                      " AND friend_id=" + std::to_string(b);
    int st = -2;
    db.query(sql, [&](const std::vector<std::string>& row) {
        st = std::atoi(row[0].c_str());
        return false;  // 只取一行
    });
    return st;
}

}  // namespace

bool user_exists(Db& db, uint32_t user_id) {
    std::string sql = "SELECT 1 FROM users WHERE user_id=" + std::to_string(user_id);
    bool found = false;
    db.query(sql, [&](const std::vector<std::string>&) {
        found = true;
        return false;
    });
    return found;
}

int register_user(Db& db, const std::string& username, const std::string& password,
                  const std::string& nickname, uint32_t& user_id) {
    // 1. 用户名查重
    std::string sel = "SELECT user_id FROM users WHERE username='" + db.escape(username) + "'";
    bool exists = false;
    if (!db.query(sel, [&](const std::vector<std::string>&) {
            exists = true;
            return false;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    if (exists) return protocol::user::ERR_USER_EXISTS;

    // 2. 插入
    std::string sql = "INSERT INTO users(username,password,nickname,created_at,online) VALUES('" +
                      db.escape(username) + "','" + db.escape(password) + "','" +
                      db.escape(nickname) + "'," + std::to_string(now_sec()) + ",0)";
    unsigned long long last_id = 0;
    if (!db.execute(sql, nullptr, &last_id)) return protocol::user::ERR_SYSTEM;
    user_id = static_cast<uint32_t>(last_id);

    // 3. 注册即自动成为自己的好友: 插入 (user_id, user_id, status=1) 记录,
    //    该关系无法通过 friend_del 删除(见 friend_del 的自引用拦截)。
    std::string self_friend =
        "INSERT INTO friends(user_id,friend_id,status,remark,ts) VALUES(" +
        std::to_string(user_id) + "," + std::to_string(user_id) + ",1,'self'," +
        std::to_string(now_sec()) + ")";
    if (!db.execute(self_friend)) return protocol::user::ERR_SYSTEM;

    return protocol::user::ERR_SUCCESS;
}

int login_user(Db& db, const std::string& username, const std::string& password,
               protocol::user::UserInfo& info) {
    std::string sql = "SELECT user_id,nickname,created_at,online FROM users WHERE username='" +
                      db.escape(username) + "' AND password='" + db.escape(password) + "'";
    bool found = false;
    if (!db.query(sql, [&](const std::vector<std::string>& row) {
            found = true;
            info.set_user_id(static_cast<uint32_t>(std::strtoul(row[0].c_str(), nullptr, 10)));
            info.set_nickname(row[1]);
            info.set_created_at(std::strtoull(row[2].c_str(), nullptr, 10));
            info.set_online(row[3] == "1");
            return false;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    if (!found) return protocol::user::ERR_INVALID_USER;

    std::string up = "UPDATE users SET online=1 WHERE user_id=" + std::to_string(info.user_id());
    if (!db.execute(up)) return protocol::user::ERR_SYSTEM;
    info.set_online(true);
    return protocol::user::ERR_SUCCESS;
}

int logout_user(Db& db, uint32_t user_id) {
    std::string sql = "UPDATE users SET online=0 WHERE user_id=" + std::to_string(user_id);
    if (!db.execute(sql)) return protocol::user::ERR_SYSTEM;
    return protocol::user::ERR_SUCCESS;
}

int verify_user(Db& db, const std::string& username, const std::string& password,
                uint32_t& user_id) {
    user_id = 0;
    std::string sel = "SELECT user_id FROM users WHERE username='" + db.escape(username) +
                      "' AND password='" + db.escape(password) + "'";
    if (!db.query(sel, [&](const std::vector<std::string>& row) {
            user_id = static_cast<uint32_t>(std::strtoul(row[0].c_str(), nullptr, 10));
            return false;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    if (user_id == 0) return protocol::user::ERR_INVALID_USER;
    return protocol::user::ERR_SUCCESS;
}

int cancel_user(Db& db, uint32_t user_id) {
    // 删除关联数据(双向)与账号本身
    std::string u = std::to_string(user_id);
    std::string dels[] = {
        "DELETE FROM friends WHERE user_id=" + u + " OR friend_id=" + u,
        "DELETE FROM blocks WHERE blocker_id=" + u + " OR blockee_id=" + u,
        "DELETE FROM messages WHERE from_id=" + u + " OR to_id=" + u,
        "DELETE FROM users WHERE user_id=" + u,
    };
    for (const auto& sql : dels) {
        if (!db.execute(sql)) return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

int friend_request(Db& db, uint32_t from_id, uint32_t to_id, const std::string& remark) {
    if (from_id == 0 || to_id == 0 || from_id == to_id) return protocol::user::ERR_INVALID_PARAM;
    if (!user_exists(db, to_id)) return protocol::user::ERR_INVALID_USER;     
 
    int a2b = rel_status(db, from_id, to_id); 
    int b2a = rel_status(db, to_id, from_id);
    if (a2b == 1 || b2a == 1) return protocol::user::ERR_ALREADY_FRIEND;
    if (a2b == 0) return protocol::user::ERR_REQUEST_PENDING;
    if (friend_is_blocked(db, from_id, to_id)) return protocol::user::ERR_BLOCKED;

    std::string sql = "INSERT INTO friends(user_id,friend_id,status,remark,ts) VALUES(" +
                      std::to_string(from_id) + "," + std::to_string(to_id) + ",0,'" +
                      db.escape(remark) + "'," + std::to_string(now_sec()) + ")";
    if (!db.execute(sql)) return protocol::user::ERR_SYSTEM;
    return protocol::user::ERR_SUCCESS; 
}

int friend_pending_list(Db& db, uint32_t user_id,
                        std::vector<protocol::user::FriendPendingItem>& items) {
    std::string sql =
        "SELECT f.user_id, u.nickname, f.remark, f.ts"
        " FROM friends f JOIN users u ON u.user_id = f.user_id"
        " WHERE f.friend_id=" + std::to_string(user_id) + " AND f.status=0"
        " ORDER BY f.ts ASC";
    if (!db.query(sql, [&](const std::vector<std::string>& row) {
            auto& item = items.emplace_back();
            item.set_friend_id(static_cast<uint32_t>(std::strtoul(row[0].c_str(), nullptr, 10)));
            item.set_nickname(row[1]);
            item.set_remark(row[2]);
            item.set_ts(std::strtoull(row[3].c_str(), nullptr, 10));
            return true;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

int friend_del(Db& db, uint32_t user_id, uint32_t friend_id) {
    // 不允许删除与自身的好友关系(注册时自动建立)
    if (user_id == friend_id) return protocol::user::ERR_INVALID_PARAM;
    std::string sql =
        "DELETE FROM friends WHERE (user_id=" + std::to_string(user_id) +
        " AND friend_id=" + std::to_string(friend_id) + ")" +
        " OR (user_id=" + std::to_string(friend_id) + " AND friend_id=" + std::to_string(user_id) + ")";
    if (!db.execute(sql)) return protocol::user::ERR_SYSTEM;
    return protocol::user::ERR_SUCCESS;
}

int friend_check(Db& db, uint32_t user_id, uint32_t friend_id,
                 bool& is_friend, std::string& nickname) {
    is_friend = false;
    nickname.clear();
    std::string sql = 
        "SELECT u.nickname FROM friends f JOIN users u ON u.user_id = f.friend_id"
        " WHERE f.user_id=" + std::to_string(user_id) +
        " AND f.friend_id=" + std::to_string(friend_id) + " AND f.status=1";
    if (!db.query(sql, [&](const std::vector<std::string>& row) {
            is_friend = true;
            nickname  = row[0];
            return false;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

int friend_block(Db& db, uint32_t user_id, uint32_t friend_id, bool block) {
    if (block) {
        // 拉黑: 只写 blocks 表, 不碰 friends —— 好友关系保持不受影响,
        // 拉黑仅作为消息发送闸门(见 friend_is_blocked)。
        std::string sql = "INSERT IGNORE INTO blocks(blocker_id,blockee_id,ts) VALUES(" +
                          std::to_string(user_id) + "," + std::to_string(friend_id) + "," +
                          std::to_string(now_sec()) + ")";
        if (!db.execute(sql)) return protocol::user::ERR_SYSTEM;
    } else {
        // 取消拉黑: 删除 blocks 记录
        std::string sql = "DELETE FROM blocks WHERE blocker_id=" + std::to_string(user_id) +
                          " AND blockee_id=" + std::to_string(friend_id);
        if (!db.execute(sql)) return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

bool friend_is_blocked(Db& db, uint32_t user_id, uint32_t peer_id) {
    // 任一方向存在拉黑记录即视为被拦(用于聊天发送闸门)
    std::string sql =
        "SELECT 1 FROM blocks WHERE (blocker_id=" + std::to_string(user_id) +
        " AND blockee_id=" + std::to_string(peer_id) + ")" +
        " OR (blocker_id=" + std::to_string(peer_id) +
        " AND blockee_id=" + std::to_string(user_id) + ") LIMIT 1";
    bool blocked = false;
    db.query(sql, [&](const std::vector<std::string>&) {
        blocked = true;
        return false;
    });
    return blocked;
}

bool friend_are_friends(Db& db, uint32_t user_id, uint32_t peer_id) {
    // 任一方向存在 status=1 即视为好友(接受时双方向都写 status=1)
    std::string sql =
        "SELECT 1 FROM friends WHERE status=1 AND (user_id=" + std::to_string(user_id) +
        " AND friend_id=" + std::to_string(peer_id) +
        " OR user_id=" + std::to_string(peer_id) +
        " AND friend_id=" + std::to_string(user_id) + ") LIMIT 1";
    bool found = false;
    db.query(sql, [&](const std::vector<std::string>&) {
        found = true;
        return false;
    });
    return found;
}

bool friend_accept_by_chat(Db& db, uint32_t from_id, uint32_t to_id) {
    // 规则: 存在 to → from 的 pending 申请, 由 from 给 to 发私聊即接受
    if (rel_status(db, to_id, from_id) != 0) return false;

    std::string up = "UPDATE friends SET status=1 WHERE user_id=" + std::to_string(to_id) +
                     " AND friend_id=" + std::to_string(from_id) + " AND status=0";
    if (!db.execute(up)) return false;

    std::string ins =
        "INSERT INTO friends(user_id,friend_id,status,remark,ts) VALUES(" +
        std::to_string(from_id) + "," + std::to_string(to_id) + ",1,''," +
        std::to_string(now_sec()) + ") ON DUPLICATE KEY UPDATE status=1";
    if (!db.execute(ins)) return false;
    return true;
}

int friend_ids(Db& db, uint32_t user_id, std::vector<uint32_t>& out) {
    out.clear();
    std::string sql =
        "SELECT friend_id FROM friends WHERE user_id=" + std::to_string(user_id) +
        " AND status=1 ORDER BY friend_id";
    if (!db.query(sql, [&](const std::vector<std::string>& row) {
            out.push_back(static_cast<uint32_t>(std::strtoul(row[0].c_str(), nullptr, 10)));
            return true;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

bool get_nickname(Db& db, uint32_t user_id, std::string& out) {
    out.clear();
    std::string sql = "SELECT nickname FROM users WHERE user_id=" + std::to_string(user_id);
    bool found = false;
    db.query(sql, [&](const std::vector<std::string>& row) {
        found = true;
        out = row[0];
        return false;
    });
    return found;
}

}  // namespace user
}  // namespace db
