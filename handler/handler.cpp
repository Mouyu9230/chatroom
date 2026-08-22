#include "handler.hpp"
#include "handler_internal.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../protocol/protocol.hpp"
#include "network/thread_pool/task_queue.hpp"

// ============================================================
//  handler 入口: 业务分派 + 共享工具。
//  业务按域拆分到 user/chat/group_handler.cpp; 本文件只保留组包工具、
//  make_system_notify(跨域系统通知) 和按 hdr->type 域分派的 handle_task。
// ============================================================

namespace handler {
namespace detail {

// 组装完整数据包: 8 字节头(header)+ protobuf body。
// type 自 v2 起为域(protocol::Domain)。
std::vector<char> build_packet(uint8_t type, const google::protobuf::Message& body) {
    std::string body_str;
    body.SerializeToString(&body_str);

    std::vector<char> packet;
    packet.resize(sizeof(protocol::packet_header) + body_str.size());

    auto* hdr = reinterpret_cast<protocol::packet_header*>(packet.data());
    hdr->magic    = protocol::MAGIC_NUM;
    hdr->ver      = 1;
    hdr->type     = type;
    hdr->body_len = static_cast<uint32_t>(body_str.size());

    if (!body_str.empty()) {
        std::memcpy(packet.data() + sizeof(protocol::packet_header),
                    body_str.data(), body_str.size());
    }
    return packet;
}

std::vector<char> user_packet(const protocol::user::UserPacket& pkt) {
    return build_packet(protocol::DOMAIN_USER, pkt);
}

std::vector<char> chat_packet(const protocol::chat::ChatPacket& pkt) {
    return build_packet(protocol::DOMAIN_CHAT, pkt);
}

std::vector<char> group_packet(const protocol::group::GroupPacket& pkt) {
    return build_packet(protocol::DOMAIN_GROUP, pkt);
}

std::vector<char> file_packet(const protocol::file::FilePacket& pkt) {
    return build_packet(protocol::DOMAIN_FILE, pkt);
}

}  // namespace detail

// ------------------------------------------------------------
//  系统通知推送 (server -> client)
//
//  与 on_chat_send 的 ChatNotify 推送采用相同构造方式:
//  填 SystemNotify → 包 UserPacket → 产出 PendingPush。
//  纯推送不落库: 目标在线即实时送达, 离线则丢弃。
// ------------------------------------------------------------
PendingPush make_system_notify(uint32_t uid, const std::string& content) {
    protocol::user::UserPacket pkt;
    auto* n = pkt.mutable_system_notify();
    n->set_content(content);
    return {uid, detail::user_packet(pkt)};
}

// ------------------------------------------------------------
TaskResult handle_task(const Task& task) {
    if (task.data.size() < sizeof(protocol::packet_header)) {
        fprintf(stderr, "[handler] packet too small: %zu\n", task.data.size());
        return {task.fd, {}, true};
    }

    const auto* hdr = reinterpret_cast<const protocol::packet_header*>(task.data.data());
    if (hdr->magic != protocol::MAGIC_NUM) {
        fprintf(stderr, "[handler] bad magic: 0x%04x\n", hdr->magic);
        return {task.fd, {}, true};
    }

    const char*  body      = task.data.data() + sizeof(protocol::packet_header);
    const size_t body_len = task.data.size() - sizeof(protocol::packet_header);

    switch (hdr->type) {
        case protocol::DOMAIN_USER:
            return detail::on_user_packet(task, body, body_len);
        case protocol::DOMAIN_CHAT:
            return detail::on_chat_packet(task, body, body_len);
        case protocol::DOMAIN_GROUP:
            return detail::on_group_packet(task, body, body_len);
        case protocol::DOMAIN_FILE:
            return detail::on_file_packet(task, body, body_len);
        default:
            fprintf(stderr, "[handler] unknown domain: 0x%02x\n", hdr->type);
            return {task.fd, {}, true};
    }
}

}  // namespace handler
