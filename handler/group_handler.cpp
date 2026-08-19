#include "handler_internal.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../protocol/group/group.pb.h"
#include "../database/db_pool.hpp"
#include "../database/user_db.hpp"
#include "../database/group_db.hpp"
#include "network/thread_pool/task_queue.hpp"

// ============================================================
//  group 域业务实现 —— 群组管理与加群审批。
//
//  与 group.proto 的 GroupPacket oneof 对齐: on_group_packet 按 body_case()
//  分发。权限检查内置在 db::group 层(acting_uid); 各 handler 在成功后
//  用 make_system_notify 推送事件通知(加群/提权/移除/解散)。
//  响应包用 detail::group_packet 组装。
// ============================================================
namespace handler {
namespace {

TaskResult on_group_create(const Task& task, const protocol::group::CreateGroupRequest& req) {
    protocol::group::GroupPacket resp;
    auto* r = resp.mutable_create_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::group_packet(resp), false};
    }
    DbGuard g(db_pool());
    uint32_t group_id = 0;
    int err = protocol::user::ERR_SYSTEM;
    if (g) err = db::group::create_group(*g, task.user_id, req.name(), group_id);
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    r->set_group_id(group_id);
    fprintf(stdout, "[handler] group create: user=%u name=%s -> group_id=%u err=%d\n",
            task.user_id, req.name().c_str(), group_id, err);
    return {task.fd, detail::group_packet(resp), false};
}

TaskResult on_group_dissolve(const Task& task, const protocol::group::DissolveGroupRequest& req) {
    protocol::group::GroupPacket resp;
    auto* r = resp.mutable_dissolve_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::group_packet(resp), false};
    }
    DbGuard g(db_pool());
    if (!g) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::group_packet(resp), false};
    }
    // 先取群名与成员(解散后表被清空, 通知需用快照)
    std::string name;
    db::group::group_name(*g, req.group_id(), name);
    std::vector<uint32_t> members;
    db::group::member_ids(*g, req.group_id(), members);

    int err = db::group::dissolve_group(*g, task.user_id, req.group_id());
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    r->set_group_id(req.group_id());
    TaskResult result{task.fd, detail::group_packet(resp), false};
    if (err == protocol::user::ERR_SUCCESS) {
        std::string notice = "群 " + name + " 已解散";
        for (uint32_t m : members) {
            if (m == task.user_id) continue;
            result.pushes.push_back(make_system_notify(m, notice));
        }
    }
    return result;
}

TaskResult on_group_promote_admin(const Task& task, const protocol::group::PromoteAdminRequest& req) {
    protocol::group::GroupPacket resp;
    auto* r = resp.mutable_promote_admin_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::group_packet(resp), false};
    }
    DbGuard g(db_pool());
    if (!g) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::group_packet(resp), false};
    }
    std::string name;
    db::group::group_name(*g, req.group_id(), name);
    int err = db::group::promote_admin(*g, task.user_id, req.group_id(), req.user_id());
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    r->set_group_id(req.group_id());
    r->set_user_id(req.user_id());
    TaskResult result{task.fd, detail::group_packet(resp), false};
    if (err == protocol::user::ERR_SUCCESS) {
        result.pushes.push_back(
            make_system_notify(req.user_id(), "你已被提升为群 " + name + " 的管理员"));
    }
    return result;
}

TaskResult on_group_join(const Task& task, const protocol::group::JoinGroupRequest& req) {
    protocol::group::GroupPacket resp;
    auto* r = resp.mutable_join_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::group_packet(resp), false};
    }
    DbGuard g(db_pool());
    if (!g) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::group_packet(resp), false};
    }
    std::string name;
    db::group::group_name(*g, req.group_id(), name);
    int err = db::group::join_request(*g, task.user_id, req.group_id(), req.remark());
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    r->set_group_id(req.group_id());
    TaskResult result{task.fd, detail::group_packet(resp), false};
    // 仅申请成功时通知群主/管理员: "<昵称> 申请加入群"
    if (err == protocol::user::ERR_SUCCESS) {
        std::string nick;
        db::user::get_nickname(*g, task.user_id, nick);
        std::string notice = nick + " 申请加入群 " + name +
                             " (user_id=" + std::to_string(task.user_id) + ")";
        std::vector<uint32_t> managers;
        if (db::group::manager_ids(*g, req.group_id(), managers) == protocol::user::ERR_SUCCESS) {
            for (uint32_t mid : managers) {
                if (mid == task.user_id) continue;
                result.pushes.push_back(make_system_notify(mid, notice));
            }
        }
    }
    return result;
}

TaskResult on_group_approve_join(const Task& task, const protocol::group::ApproveJoinRequest& req) {
    protocol::group::GroupPacket resp;
    auto* r = resp.mutable_approve_join_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::group_packet(resp), false};
    }
    DbGuard g(db_pool());
    if (!g) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::group_packet(resp), false};
    }
    std::string name;
    db::group::group_name(*g, req.group_id(), name);
    int err = db::group::approve_join(*g, task.user_id, req.group_id(), req.user_id());
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    r->set_group_id(req.group_id());
    r->set_user_id(req.user_id());
    TaskResult result{task.fd, detail::group_packet(resp), false};
    if (err == protocol::user::ERR_SUCCESS) {
        result.pushes.push_back(make_system_notify(req.user_id(), "你已加入群 " + name));
    }
    return result;
}

TaskResult on_group_reject_join(const Task& task, const protocol::group::RejectJoinRequest& req) {
    protocol::group::GroupPacket resp;
    auto* r = resp.mutable_reject_join_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::group_packet(resp), false};
    }
    DbGuard g(db_pool());
    if (!g) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::group_packet(resp), false};
    }
    std::string name;
    db::group::group_name(*g, req.group_id(), name);
    int err = db::group::reject_join(*g, task.user_id, req.group_id(), req.user_id());
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    r->set_group_id(req.group_id());
    r->set_user_id(req.user_id());
    TaskResult result{task.fd, detail::group_packet(resp), false};
    if (err == protocol::user::ERR_SUCCESS) {
        result.pushes.push_back(
            make_system_notify(req.user_id(), "你加入群 " + name + " 的申请被拒绝"));
    }
    return result;
}

TaskResult on_group_remove_member(const Task& task, const protocol::group::RemoveMemberRequest& req) {
    protocol::group::GroupPacket resp;
    auto* r = resp.mutable_remove_member_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::group_packet(resp), false};
    }
    DbGuard g(db_pool());
    if (!g) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::group_packet(resp), false};
    }
    std::string name;
    db::group::group_name(*g, req.group_id(), name);
    int err = db::group::remove_member(*g, task.user_id, req.group_id(), req.user_id());
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    r->set_group_id(req.group_id());
    r->set_user_id(req.user_id());
    TaskResult result{task.fd, detail::group_packet(resp), false};
    if (err == protocol::user::ERR_SUCCESS) {
        result.pushes.push_back(make_system_notify(req.user_id(), "你已被移出群 " + name));
    }
    return result;
}

TaskResult on_group_pending_list(const Task& task, const protocol::group::GroupPendingListRequest& req) {
    protocol::group::GroupPacket resp;
    auto* r = resp.mutable_pending_list_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::group_packet(resp), false};
    }
    DbGuard g(db_pool());
    int err = protocol::user::ERR_SYSTEM;
    std::vector<protocol::group::GroupPendingItem> items;
    if (g) err = db::group::pending_list(*g, task.user_id, req.group_id(), items);
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    for (auto& it : items) *r->mutable_items()->Add() = it;
    return {task.fd, detail::group_packet(resp), false};
}

TaskResult on_group_member_list(const Task& task, const protocol::group::GroupMemberListRequest& req) {
    protocol::group::GroupPacket resp;
    auto* r = resp.mutable_member_list_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::group_packet(resp), false};
    }
    DbGuard g(db_pool());
    int err = protocol::user::ERR_SYSTEM;
    std::vector<protocol::group::GroupMemberItem> members;
    if (g) err = db::group::member_list(*g, task.user_id, req.group_id(), members);
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    for (auto& it : members) *r->mutable_members()->Add() = it;
    return {task.fd, detail::group_packet(resp), false};
}

TaskResult on_group_list(const Task& task, const protocol::group::GroupListRequest&) {
    protocol::group::GroupPacket resp;
    auto* r = resp.mutable_group_list_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::group_packet(resp), false};
    }
    DbGuard g(db_pool());
    int err = protocol::user::ERR_SYSTEM;
    std::vector<protocol::group::GroupListItem> groups;
    if (g) err = db::group::my_groups(*g, task.user_id, groups);
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    for (auto& it : groups) *r->mutable_groups()->Add() = it;
    return {task.fd, detail::group_packet(resp), false};
}

}  // namespace

namespace detail {

TaskResult on_group_packet(const Task& task, const char* body, size_t body_len) {
    protocol::group::GroupPacket pkt;
    if (!pkt.ParseFromArray(body, static_cast<int>(body_len))) {
        fprintf(stderr, "[handler] bad group packet body\n");
        return {task.fd, {}, true};
    }
    switch (pkt.body_case()) {
        case protocol::group::GroupPacket::kCreateReq:
            return on_group_create(task, pkt.create_req());
        case protocol::group::GroupPacket::kDissolveReq:
            return on_group_dissolve(task, pkt.dissolve_req());
        case protocol::group::GroupPacket::kPromoteAdminReq:
            return on_group_promote_admin(task, pkt.promote_admin_req());
        case protocol::group::GroupPacket::kJoinReq:
            return on_group_join(task, pkt.join_req());
        case protocol::group::GroupPacket::kApproveJoinReq:
            return on_group_approve_join(task, pkt.approve_join_req());
        case protocol::group::GroupPacket::kRejectJoinReq:
            return on_group_reject_join(task, pkt.reject_join_req());
        case protocol::group::GroupPacket::kRemoveMemberReq:
            return on_group_remove_member(task, pkt.remove_member_req());
        case protocol::group::GroupPacket::kPendingListReq:
            return on_group_pending_list(task, pkt.pending_list_req());
        case protocol::group::GroupPacket::kMemberListReq:
            return on_group_member_list(task, pkt.member_list_req());
        case protocol::group::GroupPacket::kGroupListReq:
            return on_group_list(task, pkt.group_list_req());
        default:
            fprintf(stderr, "[handler] unknown group packet case: %d\n",
                    static_cast<int>(pkt.body_case()));
            return {task.fd, {}, true};
    }
}

}  // namespace detail
}  // namespace handler
