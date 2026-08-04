#pragma once

#include <cstdint>
#include <vector>

#include "../protocol/protocol.hpp"

namespace client {

// ============================================================
//  客户端协议层 —— 与 server 共用同一套 framing
//
//  数据包布局: packet_header(8B) + protobuf body, 与 handler 一致:
//     magic(2) + ver(1) + type(1) + body_len(4) + body
// ============================================================

/// 构建一个完整数据包(header + protobuf body)
std::vector<char> build_packet(uint8_t type, const google::protobuf::Message& body);

/// 阻塞发送完整数据包, 成功返回 true
bool send_packet(int fd, const std::vector<char>& packet);

/// 阻塞读取一个完整数据包(header + body), 失败/对端关闭返回空 vector
std::vector<char> read_packet(int fd);

/// 发送一个请求并等待对应类型的响应。
/// 期间收到的其它类型数据包(如系统通知)只打印并跳过。
/// 成功返回 true, resp_packet 保存完整响应包(header + body)。
bool request(int fd, uint8_t req_type, const google::protobuf::Message& req,
             uint8_t expect_type, std::vector<char>& resp_packet);

}  // namespace client
