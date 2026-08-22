#pragma once

#include <cstdint>
#include <vector>

#include "../protocol/protocol.hpp"
#include "../protocol/user/user.pb.h"
#include "../protocol/chat/chat.pb.h"
#include "../protocol/group/group.pb.h"
#include "../protocol/file/file.pb.h"

namespace client {

// ============================================================
//  客户端协议层 —— 与 server 共用同一套 framing。
//  数据包: packet_header(8B: magic+ver+type+body_len) + protobuf body; type 为域。
//  读模型: 后台收包线程(start_reader)常驻读 socket, 推送实时打印, 响应入队供 *_request 取回。
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

/// 发送 group 域请求(信封), 等待匹配的响应信封。
bool group_request(int fd, protocol::group::GroupPacket& req, protocol::group::GroupPacket& resp);

/// 发送 file 域请求(信封), 等待匹配的响应信封。
bool file_request(int fd, protocol::file::FilePacket& req, protocol::file::FilePacket& resp);

}  // namespace client
