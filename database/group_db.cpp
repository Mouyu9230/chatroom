#include "group_db.hpp"

#include <chrono>
#include <cstdlib>

#include "protocol/chat/chat.pb.h"   // TARGET_TYPE_GROUP

namespace db {
namespace group {

namespace {

uint64_t now_sec() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// 是否存在待审批加群申请。
bool has_pending_app(Db& db, uint32_t group_id, uint32_t user_id) {
    std::string sql = "SELECT 1 FROM group_applications WHERE group_id=" +
                      std::to_string(group_id) + " AND user_id=" + std::to_string(user_id);
    bool found = false;
    db.query(sql, [&](const std::vector<std::string>&) {
        found = true;
        return false;
    });
    return found;
}

}  // namespace

bool group_exists(Db& db, uint32_t group_id) {
    std::string sql = "SELECT 1 FROM group_info WHERE group_id=" + std::to_string(group_id);
    bool found = false;
    db.query(sql, [&](const std::vector<std::string>&) {
        found = true;
        return false;
    });
    return found;
}

bool group_name(Db& db, uint32_t group_id, std::string& out) {
    out.clear();
    std::string sql = "SELECT name FROM group_info WHERE group_id=" + std::to_string(group_id);
    bool found = false;
    db.query(sql, [&](const std::vector<std::string>& row) {
        found = true;
        out = row[0];
        return false;
    });
    return found;
}

int member_role(Db& db, uint32_t group_id, uint32_t user_id) {
    std::string sql = "SELECT role FROM group_members WHERE group_id=" +
                      std::to_string(group_id) + " AND user_id=" + std::to_string(user_id);
    int role = 0;   // 0 = 非成员
    db.query(sql, [&](const std::vector<std::string>& row) {
        role = std::atoi(row[0].c_str());
        return false;
    });
    return role;
}

int create_group(Db& db, uint32_t owner_id, const std::string& name, uint32_t& group_id) {
    if (name.empty() || name.size() > 64) return protocol::user::ERR_INVALID_PARAM;

    std::string sql = "INSERT INTO group_info(name,owner_id,created_at) VALUES('" +
                      db.escape(name) + "'," + std::to_string(owner_id) + "," +
                      std::to_string(now_sec()) + ")";
    unsigned long long last_id = 0;
    if (!db.execute(sql, nullptr, &last_id)) return protocol::user::ERR_SYSTEM;
    group_id = static_cast<uint32_t>(last_id);

    // 创建者自动成为群主
    std::string owner_member =
        "INSERT INTO group_members(group_id,user_id,role) VALUES(" +
        std::to_string(group_id) + "," + std::to_string(owner_id) + "," +
        std::to_string(protocol::group::GROUP_ROLE_OWNER) + ")";
    if (!db.execute(owner_member)) return protocol::user::ERR_SYSTEM;
    return protocol::user::ERR_SUCCESS;
}

int dissolve_group(Db& db, uint32_t acting_uid, uint32_t group_id) {
    if (!group_exists(db, group_id)) return protocol::user::ERR_GROUP_NOT_FOUND;
    int role = member_role(db, group_id, acting_uid);
    if (role == 0) return protocol::user::ERR_NOT_GROUP_MEMBER;
    if (role != protocol::group::GROUP_ROLE_OWNER) return protocol::user::ERR_NOT_GROUP_OWNER;

    std::string g = std::to_string(group_id);
    const std::string dels[] = {
        "DELETE FROM group_applications WHERE group_id=" + g,
        "DELETE FROM group_members WHERE group_id=" + g,
        "DELETE FROM messages WHERE to_id=" + g + " AND to_type=" +
            std::to_string(protocol::chat::TARGET_TYPE_GROUP),
        "DELETE FROM group_info WHERE group_id=" + g,
    };
    for (const auto& d : dels) {
        if (!db.execute(d)) return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

int promote_admin(Db& db, uint32_t acting_uid, uint32_t group_id, uint32_t target_uid) {
    if (!group_exists(db, group_id)) return protocol::user::ERR_GROUP_NOT_FOUND;
    int acting_role = member_role(db, group_id, acting_uid);
    if (acting_role == 0) return protocol::user::ERR_NOT_GROUP_MEMBER;
    if (acting_role != protocol::group::GROUP_ROLE_OWNER) return protocol::user::ERR_NOT_GROUP_OWNER;

    int target_role = member_role(db, group_id, target_uid);
    if (target_role == 0) return protocol::user::ERR_NOT_GROUP_MEMBER;
    if (target_role != protocol::group::GROUP_ROLE_MEMBER) return protocol::user::ERR_INVALID_PARAM;

    std::string up = "UPDATE group_members SET role=" +
                     std::to_string(protocol::group::GROUP_ROLE_ADMIN) +
                     " WHERE group_id=" + std::to_string(group_id) +
                     " AND user_id=" + std::to_string(target_uid);
    if (!db.execute(up)) return protocol::user::ERR_SYSTEM;
    return protocol::user::ERR_SUCCESS;
}

int join_request(Db& db, uint32_t user_id, uint32_t group_id, const std::string& remark) {
    if (group_id == 0 || user_id == 0) return protocol::user::ERR_INVALID_PARAM;
    if (!group_exists(db, group_id)) return protocol::user::ERR_GROUP_NOT_FOUND;
    // 已是成员(含群主/管理员): 不可重复申请
    if (member_role(db, group_id, user_id) != 0) return protocol::user::ERR_ALREADY_IN_GROUP;
    if (has_pending_app(db, group_id, user_id)) return protocol::user::ERR_REQUEST_PENDING;

    std::string sql = "INSERT INTO group_applications(group_id,user_id,remark,ts) VALUES(" +
                      std::to_string(group_id) + "," + std::to_string(user_id) + ",'" +
                      db.escape(remark) + "'," + std::to_string(now_sec()) + ")";
    if (!db.execute(sql)) return protocol::user::ERR_SYSTEM;
    return protocol::user::ERR_SUCCESS;
}

int pending_list(Db& db, uint32_t acting_uid, uint32_t group_id,
                 std::vector<protocol::group::GroupPendingItem>& items) {
    if (!group_exists(db, group_id)) return protocol::user::ERR_GROUP_NOT_FOUND;
    int role = member_role(db, group_id, acting_uid);
    if (role == 0) return protocol::user::ERR_NOT_GROUP_MEMBER;
    if (role == protocol::group::GROUP_ROLE_MEMBER) return protocol::user::ERR_NOT_GROUP_ADMIN;

    std::string sql =
        "SELECT ga.user_id, u.nickname, ga.remark, ga.ts"
        " FROM group_applications ga JOIN users u ON u.user_id = ga.user_id"
        " WHERE ga.group_id=" + std::to_string(group_id) +
        " ORDER BY ga.ts ASC";
    if (!db.query(sql, [&](const std::vector<std::string>& row) {
            auto& item = items.emplace_back();
            item.set_user_id(static_cast<uint32_t>(std::strtoul(row[0].c_str(), nullptr, 10)));
            item.set_nickname(row[1]);
            item.set_remark(row[2]);
            item.set_ts(std::strtoull(row[3].c_str(), nullptr, 10));
            return true;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

int approve_join(Db& db, uint32_t acting_uid, uint32_t group_id, uint32_t applicant_uid) {
    if (!group_exists(db, group_id)) return protocol::user::ERR_GROUP_NOT_FOUND;
    int role = member_role(db, group_id, acting_uid);
    if (role == 0) return protocol::user::ERR_NOT_GROUP_MEMBER;
    if (role == protocol::group::GROUP_ROLE_MEMBER) return protocol::user::ERR_NOT_GROUP_ADMIN;
    // 已是成员(可能因并发/重复批准): 拒绝
    if (member_role(db, group_id, applicant_uid) != 0) return protocol::user::ERR_ALREADY_IN_GROUP;
    if (!has_pending_app(db, group_id, applicant_uid)) return protocol::user::ERR_REQUEST_PENDING;

    // 先入成员后删申请: 若删申请失败, 申请者已是成员, 状态自愈。
    std::string ins =
        "INSERT INTO group_members(group_id,user_id,role) VALUES(" +
        std::to_string(group_id) + "," + std::to_string(applicant_uid) + "," +
        std::to_string(protocol::group::GROUP_ROLE_MEMBER) + ")"
        " ON DUPLICATE KEY UPDATE role=role";
    if (!db.execute(ins)) return protocol::user::ERR_SYSTEM;

    std::string del = "DELETE FROM group_applications WHERE group_id=" +
                      std::to_string(group_id) + " AND user_id=" + std::to_string(applicant_uid);
    if (!db.execute(del)) return protocol::user::ERR_SYSTEM;
    return protocol::user::ERR_SUCCESS;
}

int reject_join(Db& db, uint32_t acting_uid, uint32_t group_id, uint32_t applicant_uid) {
    if (!group_exists(db, group_id)) return protocol::user::ERR_GROUP_NOT_FOUND;
    int role = member_role(db, group_id, acting_uid);
    if (role == 0) return protocol::user::ERR_NOT_GROUP_MEMBER;
    if (role == protocol::group::GROUP_ROLE_MEMBER) return protocol::user::ERR_NOT_GROUP_ADMIN;
    if (!has_pending_app(db, group_id, applicant_uid)) return protocol::user::ERR_REQUEST_PENDING;

    std::string del = "DELETE FROM group_applications WHERE group_id=" +
                      std::to_string(group_id) + " AND user_id=" + std::to_string(applicant_uid);
    if (!db.execute(del)) return protocol::user::ERR_SYSTEM;
    return protocol::user::ERR_SUCCESS;
}

int remove_member(Db& db, uint32_t acting_uid, uint32_t group_id, uint32_t target_uid) {
    if (!group_exists(db, group_id)) return protocol::user::ERR_GROUP_NOT_FOUND;
    int acting_role = member_role(db, group_id, acting_uid);
    if (acting_role == 0) return protocol::user::ERR_NOT_GROUP_MEMBER;
    if (acting_role == protocol::group::GROUP_ROLE_MEMBER) return protocol::user::ERR_NOT_GROUP_ADMIN;
    int target_role = member_role(db, group_id, target_uid);
    if (target_role == 0) return protocol::user::ERR_NOT_GROUP_MEMBER;
    // 无人能移除自己
    if (target_uid == acting_uid) return protocol::user::ERR_INVALID_PARAM;
    // 权限边界: 群主可移除任何非自己成员(含管理员); 管理员仅能移除普通成员
    if (acting_role == protocol::group::GROUP_ROLE_ADMIN &&
        target_role != protocol::group::GROUP_ROLE_MEMBER) {
        return protocol::user::ERR_INVALID_PARAM;
    }

    std::string del = "DELETE FROM group_members WHERE group_id=" +
                      std::to_string(group_id) + " AND user_id=" + std::to_string(target_uid);
    if (!db.execute(del)) return protocol::user::ERR_SYSTEM;
    return protocol::user::ERR_SUCCESS;
}

int quit_group(Db& db, uint32_t user_id, uint32_t group_id) {
    if (!group_exists(db, group_id)) return protocol::user::ERR_GROUP_NOT_FOUND;
    int role = member_role(db, group_id, user_id);
    if (role == 0) return protocol::user::ERR_NOT_GROUP_MEMBER;
    if (role == protocol::group::GROUP_ROLE_OWNER) return protocol::user::ERR_GROUP_OWNER;

    std::string del = "DELETE FROM group_members WHERE group_id=" +
                      std::to_string(group_id) + " AND user_id=" + std::to_string(user_id);
    if (!db.execute(del)) return protocol::user::ERR_SYSTEM;
    return protocol::user::ERR_SUCCESS;
}

int member_list(Db& db, uint32_t acting_uid, uint32_t group_id,
                std::vector<protocol::group::GroupMemberItem>& members) {
    if (!group_exists(db, group_id)) return protocol::user::ERR_GROUP_NOT_FOUND;
    if (member_role(db, group_id, acting_uid) == 0) return protocol::user::ERR_NOT_GROUP_MEMBER;

    std::string sql =
        "SELECT gm.user_id, u.nickname, gm.role"
        " FROM group_members gm JOIN users u ON u.user_id = gm.user_id"
        " WHERE gm.group_id=" + std::to_string(group_id) +
        " ORDER BY gm.role ASC, gm.user_id ASC";
    if (!db.query(sql, [&](const std::vector<std::string>& row) {
            auto& m = members.emplace_back();
            m.set_user_id(static_cast<uint32_t>(std::strtoul(row[0].c_str(), nullptr, 10)));
            m.set_nickname(row[1]);
            m.set_role(static_cast<protocol::group::GroupRole>(std::atoi(row[2].c_str())));
            return true;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

int my_groups(Db& db, uint32_t user_id, std::vector<protocol::group::GroupListItem>& groups) {
    std::string sql =
        "SELECT gm.group_id, gi.name, gm.role"
        " FROM group_members gm JOIN group_info gi ON gi.group_id = gm.group_id"
        " WHERE gm.user_id=" + std::to_string(user_id) +
        " ORDER BY gm.group_id ASC";
    if (!db.query(sql, [&](const std::vector<std::string>& row) {
            auto& g = groups.emplace_back();
            g.set_group_id(static_cast<uint32_t>(std::strtoul(row[0].c_str(), nullptr, 10)));
            g.set_name(row[1]);
            g.set_role(static_cast<protocol::group::GroupRole>(std::atoi(row[2].c_str())));
            return true;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

int member_ids(Db& db, uint32_t group_id, std::vector<uint32_t>& out) {
    out.clear();
    std::string sql = "SELECT user_id FROM group_members WHERE group_id=" +
                      std::to_string(group_id) + " ORDER BY user_id";
    if (!db.query(sql, [&](const std::vector<std::string>& row) {
            out.push_back(static_cast<uint32_t>(std::strtoul(row[0].c_str(), nullptr, 10)));
            return true;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

int manager_ids(Db& db, uint32_t group_id, std::vector<uint32_t>& out) {
    out.clear();
    std::string sql =
        "SELECT user_id FROM group_members WHERE group_id=" + std::to_string(group_id) +
        " AND role IN (" + std::to_string(protocol::group::GROUP_ROLE_OWNER) + "," +
        std::to_string(protocol::group::GROUP_ROLE_ADMIN) + ") ORDER BY user_id";
    if (!db.query(sql, [&](const std::vector<std::string>& row) {
            out.push_back(static_cast<uint32_t>(std::strtoul(row[0].c_str(), nullptr, 10)));
            return true;
        })) {
        return protocol::user::ERR_SYSTEM;
    }
    return protocol::user::ERR_SUCCESS;
}

}  // namespace group
}  // namespace db
