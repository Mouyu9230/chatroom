#include "handler_internal.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../protocol/chat/chat.pb.h"
#include "../database/db_pool.hpp"
#include "../database/user_db.hpp"
#include "../database/chat_db.hpp"
#include "../database/group_db.hpp"
#include "network/thread_pool/task_queue.hpp"

// ============================================================
//  chat 域业务实现 —— 1:1 私聊 + 群聊消息收发。
//
//  与 chat.proto 的 ChatPacket oneof 对齐: on_chat_packet 按 body_case()
//  分发。私聊与群聊共用 ChatSendRequest / ChatHistoryRequest, 由 to_type
//  分流: TARGET_TYPE_GROUP → on_group_send / on_group_history, 否则按 1:1。
//  响应包用 detail::chat_packet 组装。
// ============================================================
namespace handler {
namespace {

// 群聊发送: 消息落库(to_type=GROUP, to_id=group_id), 广播 ChatNotify 给
// 所有在线成员(除发送者; 离线成员由主线程按在线表丢弃, 消息已落库可拉历史)。
TaskResult on_group_send(const Task& task, const protocol::chat::ChatSendRequest& req) {
    protocol::chat::ChatPacket resp;
    auto* r = resp.mutable_send_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::chat_packet(resp), false};
    }
    DbGuard g(db_pool());
    if (!g) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::chat_packet(resp), false};
    }
    uint32_t gid = req.to_id();
    if (!db::group::group_exists(*g, gid)) {
        r->set_err(protocol::user::ERR_GROUP_NOT_FOUND);
        return {task.fd, detail::chat_packet(resp), false};
    }
    if (db::group::member_role(*g, gid, task.user_id) == 0) {
        r->set_err(protocol::user::ERR_NOT_GROUP_MEMBER);
        return {task.fd, detail::chat_packet(resp), false};
    }

    uint64_t msg_id = 0, ts = 0;
    int err = db::chat::save_message(*g, task.user_id, gid,
                                     protocol::chat::TARGET_TYPE_GROUP, req.content(),
                                     msg_id, ts);
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    r->set_msg_id(msg_id);
    r->set_server_ts(ts);
    fprintf(stdout, "[handler] group chat: user=%u -> gid=%u: %s (msg_id=%llu)\n",
            task.user_id, gid, req.content().c_str(),
            static_cast<unsigned long long>(msg_id));

    TaskResult result{task.fd, detail::chat_packet(resp), false};
    if (err == protocol::user::ERR_SUCCESS) {
        std::vector<uint32_t> members;
        if (db::group::member_ids(*g, gid, members) == protocol::user::ERR_SUCCESS) {
            for (uint32_t m : members) {
                if (m == task.user_id) continue;
                protocol::chat::ChatPacket push;
                auto* msg = push.mutable_notify()->mutable_msg();
                msg->set_msg_id(msg_id);
                msg->set_from_id(task.user_id);
                msg->set_to_id(gid);
                msg->set_to_type(protocol::chat::TARGET_TYPE_GROUP);
                msg->set_content(req.content());
                msg->set_ts(ts);
                result.pushes.push_back({m, detail::chat_packet(push)});
            }
        }
    }
    return result;
}

// 群聊历史: 仅群内成员可拉取(to_type=GROUP 的消息)。
TaskResult on_group_history(const Task& task, const protocol::chat::ChatHistoryRequest& req) {
    protocol::chat::ChatPacket resp;
    auto* r = resp.mutable_history_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::chat_packet(resp), false};
    }
    DbGuard g(db_pool());
    if (!g) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::chat_packet(resp), false};
    }
    uint32_t gid = req.target_id();
    if (!db::group::group_exists(*g, gid)) {
        r->set_err(protocol::user::ERR_GROUP_NOT_FOUND);
        return {task.fd, detail::chat_packet(resp), false};
    }
    if (db::group::member_role(*g, gid, task.user_id) == 0) {
        r->set_err(protocol::user::ERR_NOT_GROUP_MEMBER);
        return {task.fd, detail::chat_packet(resp), false};
    }

    std::vector<protocol::chat::ChatMessage> msgs;
    int err = db::chat::query_group_history(*g, gid, req.after_msg_id(), req.limit(), msgs);
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    for (auto& m : msgs) *r->mutable_messages()->Add() = m;
    return {task.fd, detail::chat_packet(resp), false};
}

TaskResult on_chat_send(const Task& task, const protocol::chat::ChatSendRequest& req) {
    protocol::chat::ChatPacket resp;
    auto* r = resp.mutable_send_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::chat_packet(resp), false};
    }

    DbGuard g(db_pool());
    if (!g) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::chat_packet(resp), false};
    }
    // 群聊: to_type=TARGET_TYPE_GROUP 时走群消息路径(成员校验/群广播)
    if (req.to_type() == protocol::chat::TARGET_TYPE_GROUP) {
        return on_group_send(task, req);
    }
    // 接收方必须存在
    if (!db::user::user_exists(*g, req.to_id())) {
        r->set_err(protocol::user::ERR_INVALID_USER);
        return {task.fd, detail::chat_packet(resp), false};
    }

    // 任一方向已拉黑: 拒绝发送, 也不触发"私聊即接受"规则
    if (db::user::friend_is_blocked(*g, task.user_id, req.to_id())) {
        r->set_err(protocol::user::ERR_BLOCKED);
        return {task.fd, detail::chat_packet(resp), false};
    }

    // 非好友禁止私聊。
    // 例外: 若 to 之前申请了 from, from 回这条私聊即视为接受好友请求,
    //      该消息本身就是建立好友关系的入口, 允许发送。
    if (!db::user::friend_are_friends(*g, task.user_id, req.to_id()) &&
        !db::user::friend_accept_by_chat(*g, task.user_id, req.to_id())) {
        r->set_err(protocol::user::ERR_NOT_FRIEND);
        return {task.fd, detail::chat_packet(resp), false};
    }

    uint64_t msg_id = 0, ts = 0;
    int err = db::chat::save_message(*g, task.user_id, req.to_id(),
                                     protocol::chat::TARGET_TYPE_USER, req.content(),
                                     msg_id, ts);
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    r->set_msg_id(msg_id);
    r->set_server_ts(ts);
    fprintf(stdout, "[handler] chat: user=%u -> %u: %s (msg_id=%llu)\n",
            task.user_id, req.to_id(), req.content().c_str(),
            static_cast<unsigned long long>(msg_id));

    TaskResult result{task.fd, detail::chat_packet(resp), false};
    if (err == protocol::user::ERR_SUCCESS) {
        // 构造 ChatNotify 推给接收方(主线程按 to_user_id 查在线表转发)
        protocol::chat::ChatPacket push;
        auto* m = push.mutable_notify()->mutable_msg();
        m->set_msg_id(msg_id);
        m->set_from_id(task.user_id);
        m->set_to_id(req.to_id());
        m->set_to_type(protocol::chat::TARGET_TYPE_USER);
        m->set_content(req.content());
        m->set_ts(ts);
        result.pushes.push_back({req.to_id(), detail::chat_packet(push)});
    }
    return result;
}

TaskResult on_chat_history(const Task& task, const protocol::chat::ChatHistoryRequest& req) {
    protocol::chat::ChatPacket resp;
    auto* r = resp.mutable_history_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::chat_packet(resp), false};
    }
    DbGuard g(db_pool());
    // 群聊历史: to_type=TARGET_TYPE_GROUP 时走群历史路径(成员校验)
    if (req.to_type() == protocol::chat::TARGET_TYPE_GROUP) {
        return on_group_history(task, req);
    }
    int err = protocol::user::ERR_SYSTEM;
    std::vector<protocol::chat::ChatMessage> msgs;
    if (g) err = db::chat::query_history(*g, task.user_id, req.target_id(),
                                         req.after_msg_id(), req.limit(), msgs);
    r->set_err(static_cast<protocol::user::ErrCode>(err));
    for (auto& m : msgs) *r->mutable_messages()->Add() = m;
    return {task.fd, detail::chat_packet(resp), false};
}

}  // namespace

namespace detail {

TaskResult on_chat_packet(const Task& task, const char* body, size_t body_len) {
    protocol::chat::ChatPacket pkt;
    if (!pkt.ParseFromArray(body, static_cast<int>(body_len))) {
        fprintf(stderr, "[handler] bad chat packet body\n");
        return {task.fd, {}, true};
    }
    switch (pkt.body_case()) {
        case protocol::chat::ChatPacket::kSendReq:
            return on_chat_send(task, pkt.send_req());
        case protocol::chat::ChatPacket::kHistoryReq:
            return on_chat_history(task, pkt.history_req());
        case protocol::chat::ChatPacket::kReadAck:
            // 已读回执: 暂无读取状态表, 直接忽略
            return {task.fd, {}, false};
        default:
            fprintf(stderr, "[handler] unknown chat packet case: %d\n",
                    static_cast<int>(pkt.body_case()));
            return {task.fd, {}, true};
    }
}

}  // namespace detail
}  // namespace handler
