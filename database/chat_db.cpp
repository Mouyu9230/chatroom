#include "chat_db.hpp"

#include <chrono>
#include <cstdlib>

namespace db {
namespace chat {

namespace {

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

}  // namespace

int save_message(Db& db, uint32_t from_id, uint32_t to_id, uint32_t to_type,
                 const std::string& content, uint64_t& msg_id, uint64_t& ts) {
    ts = now_ms();
    std::string sql = "INSERT INTO messages(from_id,to_id,to_type,content,ts) VALUES(" +
                      std::to_string(from_id) + "," + std::to_string(to_id) + "," +
                      std::to_string(to_type) + ",'" + db.escape(content) + "'," +
                      std::to_string(ts) + ")";
    unsigned long long last_id = 0;
    if (!db.execute(sql, nullptr, &last_id)) return protocol::user::ERR_SYSTEM;
    msg_id = last_id;
    return protocol::user::ERR_SUCCESS;
}

int query_history(Db& db, uint32_t self_id, uint32_t target_id, uint64_t after_msg_id,
                  uint32_t limit, std::vector<protocol::chat::ChatMessage>& out) {
    if (limit == 0 || limit > 200) limit = 50;
    // 限定 to_type=1: 群 id 与用户 id 共用 to_id 命名空间, 防止 1:1 历史串出群消息
    std::string sql =
        "SELECT msg_id,from_id,to_id,to_type,content,ts FROM messages"
        " WHERE ((from_id=" + std::to_string(self_id) + " AND to_id=" + std::to_string(target_id) + ")"
        " OR (from_id=" + std::to_string(target_id) + " AND to_id=" + std::to_string(self_id) + "))"
        " AND to_type=1"
        " AND msg_id>" + std::to_string(after_msg_id) +
        " ORDER BY msg_id DESC LIMIT " + std::to_string(limit);
    if (!db.query(sql, [&](const std::vector<std::string>& row) {
            auto& m = out.emplace_back();
            m.set_msg_id(std::strtoull(row[0].c_str(), nullptr, 10));
            m.set_from_id(static_cast<uint32_t>(std::strtoul(row[1].c_str(), nullptr, 10)));
            m.set_to_id(static_cast<uint32_t>(std::strtoul(row[2].c_str(), nullptr, 10)));
            m.set_to_type(static_cast<protocol::chat::TargetType>(std::atoi(row[3].c_str())));
            m.set_content(row[4]);
            m.set_ts(std::strtoull(row[5].c_str(), nullptr, 10));
            return true;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

int query_group_history(Db& db, uint32_t group_id, uint64_t after_msg_id,
                        uint32_t limit, std::vector<protocol::chat::ChatMessage>& out) {
    if (limit == 0 || limit > 200) limit = 50;
    std::string sql =
        "SELECT msg_id,from_id,to_id,to_type,content,ts FROM messages"
        " WHERE to_id=" + std::to_string(group_id) +
        " AND to_type=" + std::to_string(protocol::chat::TARGET_TYPE_GROUP) +
        " AND msg_id>" + std::to_string(after_msg_id) +
        " ORDER BY msg_id DESC LIMIT " + std::to_string(limit);
    if (!db.query(sql, [&](const std::vector<std::string>& row) {
            auto& m = out.emplace_back();
            m.set_msg_id(std::strtoull(row[0].c_str(), nullptr, 10));
            m.set_from_id(static_cast<uint32_t>(std::strtoul(row[1].c_str(), nullptr, 10)));
            m.set_to_id(static_cast<uint32_t>(std::strtoul(row[2].c_str(), nullptr, 10)));
            m.set_to_type(static_cast<protocol::chat::TargetType>(std::atoi(row[3].c_str())));
            m.set_content(row[4]);
            m.set_ts(std::strtoull(row[5].c_str(), nullptr, 10));
            return true;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

int offline_summary(Db& db, uint32_t self_id, uint64_t after_ts,
                    std::vector<OfflineItem>& out) {
    std::string sql =
        "SELECT m.from_id, u.nickname, COUNT(*) AS cnt"
        " FROM messages m JOIN users u ON u.user_id = m.from_id"
        " WHERE m.to_id=" + std::to_string(self_id) +
        " AND m.from_id<>" + std::to_string(self_id) +
        " AND m.ts>" + std::to_string(after_ts) +
        " AND m.to_type=1"
        " GROUP BY m.from_id, u.nickname"
        " ORDER BY cnt DESC";
    if (!db.query(sql, [&](const std::vector<std::string>& row) {
            auto& it = out.emplace_back();
            it.from_id = static_cast<uint32_t>(std::strtoul(row[0].c_str(), nullptr, 10));
            it.nickname = row[1];
            it.count = static_cast<uint32_t>(std::strtoul(row[2].c_str(), nullptr, 10));
            return true;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

}  // namespace chat
}  // namespace db
