#include "handler_internal.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../protocol/user/user.pb.h"
#include "../database/db_pool.hpp"
#include "../database/user_db.hpp"
#include "../database/chat_db.hpp"
#include "network/thread_pool/task_queue.hpp"

// ============================================================
//  user 域业务实现 —— 账号认证、心跳、好友系统、注销。
//
//  与 user.proto 的 UserPacket oneof 对齐: on_user_packet 按
//  body_case() 分发到下方各 on_* 函数。响应包用 detail::user_packet 组装。
// ============================================================
namespace handler {
namespace {

TaskResult on_register(const Task& task, const protocol::user::RegisterRequest& req) {
    protocol::user::UserPacket resp;
    auto* r = resp.mutable_register_resp();

    DbGuard g(db_pool());
    if (!g) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::user_packet(resp), false};
    }
    uint32_t user_id = 0;
    int err = db::user::register_user(*g, req.username(), req.password(), req.nickname(), user_id);
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    r->set_user_id(user_id);
    fprintf(stdout, "[handler] register username=%s -> err=%d uid=%u\n",
            req.username().c_str(), err, user_id);
    return {task.fd, detail::user_packet(resp), false};
}

TaskResult on_login(const Task& task, const protocol::user::LoginRequest& req) {
    protocol::user::UserPacket resp;
    auto* r = resp.mutable_login_resp();

    DbGuard g(db_pool());
    if (!g) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::user_packet(resp), false};
    }
    protocol::user::UserInfo info;
    uint64_t last_offline_ts = 0;
    int err = db::user::login_user(*g, req.username(), req.password(), info, last_offline_ts);
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    if (err == protocol::user::ERR_SUCCESS) {
        r->set_user_id(info.user_id());
        *r->mutable_user() = info;
        fprintf(stdout, "[handler] login ok: user=%u nick=%s\n",
                info.user_id(), info.nickname().c_str());
        // user_id 非 0: 主线程把该连接绑定为该用户
        TaskResult result{task.fd, detail::user_packet(resp), false, info.user_id()};

        std::vector<db::chat::OfflineItem> offline;
        if (db::chat::offline_summary(*g, info.user_id(), last_offline_ts, offline) ==
            protocol::user::ERR_SUCCESS) {
            for (const auto& it : offline) {
                std::string notice = it.nickname + " 离线期间给你发了 " +
                                     std::to_string(it.count) + " 条消息";
                result.pushes.push_back(make_system_notify(info.user_id(), notice));
            }
        }

        // 系统通知: 向所有好友推送"该用户已上线" (自身除外, 自加好友不通知自己)
        std::vector<uint32_t> friends;
        if (db::user::friend_ids(*g, info.user_id(), friends) == protocol::user::ERR_SUCCESS) {
            std::string notice = info.nickname() + " 已上线";
            for (uint32_t fid : friends) {
                if (fid == info.user_id()) continue;
                result.pushes.push_back(make_system_notify(fid, notice));
            }
        }
        return result;
    }
    return {task.fd, detail::user_packet(resp), false};
}

TaskResult on_logout(const Task& task, const protocol::user::LogoutRequest& req) {
    protocol::user::UserPacket resp;
    auto* r = resp.mutable_logout_resp();

    DbGuard g(db_pool());

    int err = protocol::user::ERR_SUCCESS;
    if (!g) err = protocol::user::ERR_SYSTEM;
    else err = db::user::logout_user(*g, req.user_id());
    r->set_err(static_cast<protocol::user::ErrCode>(err));

    // 返回 unbind_user: 主线程解绑在线表, 但保持连接不断
    TaskResult result{task.fd, detail::user_packet(resp), false, 0, true};

    // 系统通知: 向所有好友推送"该用户已下线"
    if (err == protocol::user::ERR_SUCCESS) {
        std::string nickname;
        db::user::get_nickname(*g, req.user_id(), nickname);
        std::vector<uint32_t> friends;
        if (db::user::friend_ids(*g, req.user_id(), friends) == protocol::user::ERR_SUCCESS) {
            std::string notice = nickname + " 已下线";
            for (uint32_t fid : friends) {
                if (fid == req.user_id()) continue;
                result.pushes.push_back(make_system_notify(fid, notice));
            }
        }
    }
    return result;
}

TaskResult on_heartbeat(const Task& task) {
    protocol::user::UserPacket resp;
    resp.mutable_heartbeat_resp();
    return {task.fd, detail::user_packet(resp), false};
}

TaskResult on_friend_request(const Task& task, const protocol::user::FriendRequestRequest& req) {
    protocol::user::UserPacket resp;
    auto* r = resp.mutable_friend_request_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::user_packet(resp), false};
    }
    DbGuard g(db_pool());
    int err = protocol::user::ERR_SYSTEM;

    if (g) err = db::user::friend_request(*g, task.user_id, req.friend_id(), req.remark());
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    r->set_friend_id(req.friend_id());
    TaskResult result = {task.fd, detail::user_packet(resp), false};

    // 仅申请成功时才向对方推送"收到好友申请"通知(失败如自加/已拉黑不打扰)
    if (g && err == protocol::user::ERR_SUCCESS) {
        std::string nick;
        db::user::get_nickname(*g, task.user_id, nick);
        std::string notice = "You received a friend request from " + nick +
                             ",user id=" + std::to_string(task.user_id);
        result.pushes.push_back(make_system_notify(req.friend_id(), notice));
    }
    return result;
}

TaskResult on_friend_pending_list(const Task& task, const protocol::user::FriendPendingListRequest&) {
    protocol::user::UserPacket resp;
    auto* r = resp.mutable_friend_pending_list_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::user_packet(resp), false};
    }
    DbGuard g(db_pool());
    int err = protocol::user::ERR_SYSTEM;
    std::vector<protocol::user::FriendPendingItem> items;
    if (g) err = db::user::friend_pending_list(*g, task.user_id, items);
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    for (auto& it : items) *r->mutable_items()->Add() = it;
    return {task.fd, detail::user_packet(resp), false};
}

TaskResult on_friend_del(const Task& task, const protocol::user::FriendDelRequest& req) {
    protocol::user::UserPacket resp;
    auto* r = resp.mutable_friend_del_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::user_packet(resp), false};
    }
    DbGuard g(db_pool());
    int err = protocol::user::ERR_SYSTEM;

    if (g) err = db::user::friend_del(*g, task.user_id, req.friend_id());
    r->set_err(static_cast<protocol::user::ErrCode>(err));

    TaskResult result = {task.fd, detail::user_packet(resp), false};

    if (g && err == protocol::user::ERR_SUCCESS) {
        std::string nick;
        db::user::get_nickname(*g, req.friend_id(), nick);
        std::string notice = nick + " userid=" + std::to_string(req.friend_id()) +
                             " have been removed from your friend list";
        result.pushes.push_back(make_system_notify(task.user_id, notice));
    }

    return result;
}

TaskResult on_friend_check(const Task& task, const protocol::user::FriendCheckRequest& req) {
    protocol::user::UserPacket resp;
    auto* r = resp.mutable_friend_check_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::user_packet(resp), false};
    }
    DbGuard g(db_pool());
    int err = protocol::user::ERR_SYSTEM;
    bool is_friend = false;
    std::string nickname;
    if (g) err = db::user::friend_check(*g, task.user_id, req.friend_id(), is_friend, nickname);
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    r->set_is_friend(is_friend);
    r->set_nickname(nickname);
    return {task.fd, detail::user_packet(resp), false};
}

TaskResult on_friend_block(const Task& task, const protocol::user::FriendBlockRequest& req) {
    protocol::user::UserPacket resp;
    auto* r = resp.mutable_friend_block_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::user_packet(resp), false};
    }
    DbGuard g(db_pool());

    int err = protocol::user::ERR_SYSTEM;
    if (g) err = db::user::friend_block(*g, task.user_id, req.friend_id(), req.block());
    r->set_err(static_cast<protocol::user::ErrCode>(err));

    TaskResult result = {task.fd, detail::user_packet(resp), false};

    if (g && err == protocol::user::ERR_SUCCESS) {
        // 区分拉黑/解除: 通知文案随 req.block() 变化, 让双方都知晓当前状态。
        std::string nick;
        const char* subject = req.block() ? "have been blocked" : "have been unblocked";
        const char* byline  = req.block() ? "You've been blocked by " : "You've been unblocked by ";
        db::user::get_nickname(*g, req.friend_id(), nick);
        std::string notice = nick + " userid=" + std::to_string(req.friend_id()) + " " + subject;
        result.pushes.push_back(make_system_notify(task.user_id, notice));
        db::user::get_nickname(*g, task.user_id, nick);
        notice = byline + nick + " userid= " + std::to_string(task.user_id);
        result.pushes.push_back(make_system_notify(req.friend_id(), notice));
    }
    return result;
}

// 注销账号: 凭据(用户名+密码)校验通过后, 删除账号及其关联数据
// (好友关系/拉黑/聊天记录), 并向其好友推送"已注销"通知。
// 若注销的正是当前连接绑定的用户, 解绑在线表(连接保持, 类似登出)。
TaskResult on_cancel(const Task& task, const protocol::user::CancelRequest& req) {
    protocol::user::UserPacket resp;
    auto* r = resp.mutable_cancel_resp();

    DbGuard g(db_pool());
    if (!g) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::user_packet(resp), false};
    }

    // 1. 凭据校验, 取得 user_id
    uint32_t user_id = 0;
    int err = db::user::verify_user(*g, req.username(), req.password(), user_id);

    TaskResult result{task.fd, detail::user_packet(resp), false};
    if (err == protocol::user::ERR_SUCCESS) {
        // 2. 删除前先取好友列表与昵称, 供注销通知推送(删除后 friends 表已清空)
        std::vector<uint32_t> friends;
        db::user::friend_ids(*g, user_id, friends);
        std::string nick;
        db::user::get_nickname(*g, user_id, nick);

        // 3. 删除账号及其关联数据
        err = db::user::cancel_user(*g, user_id);

        // 4. 向好友推送"已注销"通知(离线者由主线程丢弃; 自加好友不通知自己)
        if (err == protocol::user::ERR_SUCCESS) {
            std::string notice = nick + " 已注销";
            for (uint32_t fid : friends) {
                if (fid == user_id) continue;
                result.pushes.push_back(make_system_notify(fid, notice));
            }
            // 注销的正是当前连接绑定的用户: 解绑在线表
            if (task.user_id != 0 && task.user_id == user_id) {
                result.unbind_user = true;
            }
        }
    }
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    fprintf(stdout, "[handler] cancel username=%s -> err=%d uid=%u\n",
            req.username().c_str(), err, user_id);
    return result;
}

}  // namespace

namespace detail {

TaskResult on_user_packet(const Task& task, const char* body, size_t body_len) {
    protocol::user::UserPacket pkt;
    if (!pkt.ParseFromArray(body, static_cast<int>(body_len))) {
        fprintf(stderr, "[handler] bad user packet body\n");
        return {task.fd, {}, true};
    }
    switch (pkt.body_case()) {
        case protocol::user::UserPacket::kRegisterReq:
            return on_register(task, pkt.register_req());
        case protocol::user::UserPacket::kLoginReq:
            return on_login(task, pkt.login_req());
        case protocol::user::UserPacket::kLogoutReq:
            return on_logout(task, pkt.logout_req());
        case protocol::user::UserPacket::kHeartbeatReq:
            return on_heartbeat(task);
        case protocol::user::UserPacket::kFriendRequestReq:
            return on_friend_request(task, pkt.friend_request_req());
        case protocol::user::UserPacket::kFriendPendingListReq:
            return on_friend_pending_list(task, pkt.friend_pending_list_req());
        case protocol::user::UserPacket::kFriendDelReq:
            return on_friend_del(task, pkt.friend_del_req());
        case protocol::user::UserPacket::kFriendCheckReq:
            return on_friend_check(task, pkt.friend_check_req());
        case protocol::user::UserPacket::kFriendBlockReq:
            return on_friend_block(task, pkt.friend_block_req());
        case protocol::user::UserPacket::kCancelReq:
            return on_cancel(task, pkt.cancel_req());
        default:
            fprintf(stderr, "[handler] unknown user packet case: %d\n",
                    static_cast<int>(pkt.body_case()));
            return {task.fd, {}, true};
    }
}

}  // namespace detail
}  // namespace handler
