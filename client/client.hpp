#pragma once

#include <cstdint>
#include <vector>

#include "../protocol/protocol.hpp"
#include "../protocol/user/user.pb.h"
#include "../protocol/chat/chat.pb.h"

namespace client {

// ============================================================
//  客户端协议层 —— 与 server 共用同一套 framing
//
//  数据包布局: packet_header(8B) + protobuf body, 与 handler 一致:
//     magic(2) + ver(1) + type(1) + body_len(4) + body
//  type 为域: DOMAIN_USER → UserPacket / DOMAIN_CHAT → ChatPacket
//
//  读模型: 启动后台收包线程(start_reader)常驻读 socket,
//    推送(ChatNotify/SystemNotify/UserStatusNotify)实时打印;
//    请求响应入队, 由 user_request/chat_request 取回。
// ============================================================

/// 构建一个完整数据包(header + protobuf body)
std::vector<char> build_packet(uint8_t type, const google::protobuf::Message& body);

/// 阻塞发送完整数据包, 成功返回 true
bool send_packet(int fd, const std::vector<char>& packet);

/// 启动后台收包线程。连接关闭或出错时自动结束并唤醒所有等待者。
void start_reader(int fd);

/// 发送 user 域请求(信封), 等待匹配的响应信封(由请求分支推出)。
bool user_request(int fd, protocol::user::UserPacket& req, protocol::user::UserPacket& resp);

/// 发送 chat 域请求(信封), 等待匹配的响应信封。
bool chat_request(int fd, protocol::chat::ChatPacket& req, protocol::chat::ChatPacket& resp);

}  // namespace client
