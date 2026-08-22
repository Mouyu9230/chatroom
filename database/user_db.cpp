#include "user_db.hpp"

#include <chrono>
#include <cstdlib>
#include <string>

#include "redis.hpp"

namespace db {
namespace user {

namespace {

uint64_t now_sec() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// 毫秒时间戳, 与 messages.ts 同单位, 用于离线水位 last_offline_ts。
uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
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

// ============================================================
//  Redis 热读缓存(cache-aside) —— 仅做加速, 不改变语义
//
//  覆盖三个高频读: 昵称 / 是否拉黑 / 是否好友。
//    - 读: 先查 Redis, 未命中再走 MySQL 并回填(TTL 兜底);
//    - 写: 相关写函数在 MySQL 成功后显式失效(见各函数);
//    - 降级: Redis 不可用(RedisGuard 为空 / get 出错)一律回退 MySQL。
//
//  key 规约:
//    cr:nick:{uid}          -> nickname,   TTL 1h
//    cr:block:{lo}:{hi}     -> "1"/"0",    TTL 30s(双向共用)
//    cr:friend:{lo}:{hi}    -> "1"/"0",    TTL 30s(双向共用, status=1 判定)
//  拉黑/好友判定本身对称, 故键按 (min,max) 规范化, 双向共用一个键,
//  失效一次即可覆盖两个方向。
// ============================================================
constexpr long kNickTtl   = 3600;
constexpr long kRelTtl    = 30;  // 短 TTL: cache-aside 写回与失效之间的竞态
                                 // 造成的陈旧结果最多存活一个 TTL, 聊天场景可接受。
const char kNickPrefix[]   = "cr:nick:";
const char kBlockPrefix[]  = "cr:block:";
const char kFriendPrefix[] = "cr:friend:";

std::string nick_key(uint32_t uid) {
    return std::string(kNickPrefix) + std::to_string(uid);
}
std::string pair_key(const char* prefix, uint32_t a, uint32_t b) {
    uint32_t lo = a < b ? a : b;
    uint32_t hi = a < b ? b : a;
    return std::string(prefix) + std::to_string(lo) + ":" + std::to_string(hi);
}

// 读昵称缓存: 命中返回 true 且 out 有效; 未命中/缓存不可用返回 false。
bool cache_get_nick(Redis* r, uint32_t uid, std::string& out) {
    if (r == nullptr) return false;
    bool found = false;
    if (!r->get(nick_key(uid), out, found)) return false;
    return found;
}
void cache_set_nick(Redis* r, uint32_t uid, const std::string& nick) {
    if (r) r->set(nick_key(uid), nick, kNickTtl);
}
void cache_invalidate_nick(Redis* r, uint32_t uid) {
    if (r) r->del(nick_key(uid));
}

// 读布尔关系缓存: 命中返回 true 且 out 有效; 未命中/缓存不可用返回 false。
bool cache_get_bool(Redis* r, const std::string& key, bool& out) {
    if (r == nullptr) return false;
    std::string v;
    bool found = false;
    if (!r->get(key, v, found) || !found) return false;
    out = (v == "1");
    return true;
}
void cache_set_bool(Redis* r, const std::string& key, bool val) {
    if (r) r->set(key, val ? "1" : "0", kRelTtl);
}
void cache_invalidate_bool(Redis* r, const std::string& key) {
    if (r) r->del(key);
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


    RedisGuard r(redis_pool());
    cache_invalidate_nick(r.get(), user_id);
    cache_invalidate_bool(r.get(), pair_key(kFriendPrefix, user_id, user_id));

    return protocol::user::ERR_SUCCESS;
}

int login_user(Db& db, const std::string& username, const std::string& password,
               protocol::user::UserInfo& info, uint64_t& last_offline_ts) {
    std::string sql = "SELECT user_id,nickname,created_at,online,last_offline_ts FROM users WHERE username='" +
                      db.escape(username) + "' AND password='" + db.escape(password) + "'";
    bool found = false;
    if (!db.query(sql, [&](const std::vector<std::string>& row) {
            found = true;
            info.set_user_id(static_cast<uint32_t>(std::strtoul(row[0].c_str(), nullptr, 10)));
            info.set_nickname(row[1]);
            info.set_created_at(std::strtoull(row[2].c_str(), nullptr, 10));
            info.set_online(row[3] == "1");
            last_offline_ts = std::strtoull(row[4].c_str(), nullptr, 10);
            return false;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    if (!found) return protocol::user::ERR_INVALID_USER;

    // 推进离线水位: 本次在线时段的离线摘要只统计到登录时刻为止。
    std::string up = "UPDATE users SET online=1, last_offline_ts=" +
                     std::to_string(now_ms()) + " WHERE user_id=" + std::to_string(info.user_id());
    if (!db.execute(up)) return protocol::user::ERR_SYSTEM;
    info.set_online(true);
    return protocol::user::ERR_SUCCESS;
}

int logout_user(Db& db, uint32_t user_id) {
    // 记录本次离线开始时刻, 供下次登录计算离线消息摘要。
    std::string sql = "UPDATE users SET online=0, last_offline_ts=" +
                      std::to_string(now_ms()) + " WHERE user_id=" + std::to_string(user_id);
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
    // 失效: 昵称缓存直接清; 涉及该用户的 friend/block 关系键由短 TTL 兜底
    // (此处不枚举其好友列表逐个失效, 保持改动最小)。
    RedisGuard r(redis_pool());
    cache_invalidate_nick(r.get(), user_id);
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
    // 同时清除双方互拉黑记录: 删除好友后再发消息应报"非好友"(ERR_NOT_FRIEND)
    // 而非"已拉黑"(ERR_BLOCKED)。
    std::string unblock =
        "DELETE FROM blocks WHERE (blocker_id=" + std::to_string(user_id) +
        " AND blockee_id=" + std::to_string(friend_id) + ")" +
        " OR (blocker_id=" + std::to_string(friend_id) + " AND blockee_id=" + std::to_string(user_id) + ")";
    if (!db.execute(unblock)) return protocol::user::ERR_SYSTEM;

    // 失效: 好友关系与拉黑状态都变了(规范化键覆盖双向)。
    RedisGuard r(redis_pool());
    cache_invalidate_bool(r.get(), pair_key(kFriendPrefix, user_id, friend_id));
    cache_invalidate_bool(r.get(), pair_key(kBlockPrefix, user_id, friend_id));
    return protocol::user::ERR_SUCCESS;
}

int friend_list(Db& db, uint32_t user_id,
                std::vector<protocol::user::FriendListItem>& out) {
    out.clear();
    std::string sql =
        "SELECT u.user_id, u.nickname, u.online FROM friends f"
        " JOIN users u ON u.user_id = f.friend_id"
        " WHERE f.user_id=" + std::to_string(user_id) +
        " AND f.status=1 AND f.friend_id<>f.user_id ORDER BY f.friend_id";
    if (!db.query(sql, [&](const std::vector<std::string>& row) {
            protocol::user::FriendListItem item;
            item.set_user_id(static_cast<uint32_t>(std::strtoul(row[0].c_str(), nullptr, 10)));
            item.set_nickname(row[1]);
            item.set_online(row[2] == "1");
            out.push_back(std::move(item));
            return true;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

int friend_block(Db& db, uint32_t user_id, uint32_t friend_id, bool block) {
    RedisGuard r(redis_pool());  // 提前取, 下方写库成功后失效缓存
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
    // 失效: 拉黑/取消拉黑都改变 is_blocked 结果(规范化键覆盖双向)。
    cache_invalidate_bool(r.get(), pair_key(kBlockPrefix, user_id, friend_id));
    return protocol::user::ERR_SUCCESS;
}

bool friend_is_blocked(Db& db, uint32_t user_id, uint32_t peer_id) {
    // 任一方向存在拉黑记录即视为被拦(用于聊天发送闸门)
    RedisGuard r(redis_pool());
    std::string key = pair_key(kBlockPrefix, user_id, peer_id);
    bool cached = false;
    if (cache_get_bool(r.get(), key, cached)) return cached;

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
    cache_set_bool(r.get(), key, blocked);
    return blocked;
}

bool friend_are_friends(Db& db, uint32_t user_id, uint32_t peer_id) {
    // 任一方向存在 status=1 即视为好友(接受时双方向都写 status=1)
    RedisGuard r(redis_pool());
    std::string key = pair_key(kFriendPrefix, user_id, peer_id);
    bool cached = false;
    if (cache_get_bool(r.get(), key, cached)) return cached;

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
    cache_set_bool(r.get(), key, found);
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

    // 失效: 好友关系由 pending 变为 accepted。
    RedisGuard r(redis_pool());
    cache_invalidate_bool(r.get(), pair_key(kFriendPrefix, from_id, to_id));
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
    RedisGuard r(redis_pool());
    if (cache_get_nick(r.get(), user_id, out)) return true;

    out.clear();
    std::string sql = "SELECT nickname FROM users WHERE user_id=" + std::to_string(user_id);
    bool found = false;
    db.query(sql, [&](const std::vector<std::string>& row) {
        found = true;
        out = row[0];
        return false;
    });
    if (found) cache_set_nick(r.get(), user_id, out);
    return found;
}

}  // namespace user
}  // namespace db
