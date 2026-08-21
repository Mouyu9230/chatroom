#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db.hpp"
#include "protocol/group/group.pb.h"

// ============================================================
//  群域持久化操作 —— 与 group.proto 对齐
//
//  返回值为 user.proto 的 ErrCode 数值(ERR_SUCCESS=0 表示成功)。
//  权限检查内置在各函数中(传入 acting_uid): 群主 > 管理员 > 普通成员。
//  member_role 返回 0 表示非群成员。
// ============================================================
namespace db {
namespace group {

// 群是否存在。
bool group_exists(Db&, uint32_t group_id);

// 群名; 群不存在返回 false。
bool group_name(Db&, uint32_t group_id, std::string& out);

// 用户在群中的角色: 1=群主 2=管理员 3=普通成员; 非成员返回 0。
int member_role(Db&, uint32_t group_id, uint32_t user_id);

// 创建群。owner 自动成为群主(role=1)。成功输出 group_id。
int create_group(Db&, uint32_t owner_id, const std::string& name, uint32_t& group_id);

// 解散群 (仅群主)。顺带清理该群的申请/成员/聊天记录。
int dissolve_group(Db&, uint32_t acting_uid, uint32_t group_id);

// 提升普通成员为管理员 (仅群主, 目标须为普通成员)。
int promote_admin(Db&, uint32_t acting_uid, uint32_t group_id, uint32_t target_uid);

// 申请加群。已是成员/已是群主 → ERR_ALREADY_IN_GROUP; 已申请 → ERR_REQUEST_PENDING。
int join_request(Db&, uint32_t user_id, uint32_t group_id, const std::string& remark);

// 待审批加群申请列表 (仅群主/管理员)。
int pending_list(Db&, uint32_t acting_uid, uint32_t group_id,
                 std::vector<protocol::group::GroupPendingItem>& items);

// 批准加群申请 (仅群主/管理员)。先入成员后删申请, 使失败自愈。
int approve_join(Db&, uint32_t acting_uid, uint32_t group_id, uint32_t applicant_uid);

// 拒绝加群申请 (仅群主/管理员): 删除申请。
int reject_join(Db&, uint32_t acting_uid, uint32_t group_id, uint32_t applicant_uid);

// 移除成员 (群主可移非自己外任何人; 管理员仅能移除普通成员)。
int remove_member(Db&, uint32_t acting_uid, uint32_t group_id, uint32_t target_uid);

// 主动退群: 成员把自己移出群。非成员 ERR_NOT_GROUP_MEMBER; 群主 ERR_GROUP_OWNER(须解散群)。
int quit_group(Db&, uint32_t user_id, uint32_t group_id);

// 群成员列表 (群内成员可查, JOIN users 取昵称)。
int member_list(Db&, uint32_t acting_uid, uint32_t group_id,
                std::vector<protocol::group::GroupMemberItem>& members);

// 我的群列表。
int my_groups(Db&, uint32_t user_id, std::vector<protocol::group::GroupListItem>& groups);

// 群内全部成员 user_id (含群主/管理员), 供群消息广播 / 解散通知。
int member_ids(Db&, uint32_t group_id, std::vector<uint32_t>& out);

// 群主 + 管理员 user_id, 供加群申请通知。
int manager_ids(Db&, uint32_t group_id, std::vector<uint32_t>& out);

}  // namespace group
}  // namespace db
