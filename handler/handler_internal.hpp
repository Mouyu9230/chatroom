#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../protocol/protocol.hpp"
#include "../protocol/user/user.pb.h"
#include "../protocol/chat/chat.pb.h"
#include "../protocol/group/group.pb.h"
#include "handler.hpp"

// ============================================================
//  handler 内部共享工具 —— 仅供 handler 各域实现文件使用。
//
//  handler 模块按域拆分为 user/chat/group 三个实现文件, 各自持有一批
//  on_* 业务函数; 本头文件只暴露它们与 handler.cpp 之间需要共享的接口:
//    - build_packet / user_packet / chat_packet / group_packet:
//      组装 8 字节头 + protobuf body, 并按域封包(定义在 handler.cpp)。
//    - on_user_packet / on_chat_packet / on_group_packet:
//      各域 oneof 分发器(定义在各域实现文件), 由 handler.cpp 的
//      handle_task 按 hdr->type 调用。
//  这些符号仅供本模块内部使用, 不对外暴露。
// ============================================================
namespace handler {
namespace detail {

/// 组装完整数据包: 8 字节头(header)+ protobuf body。
std::vector<char> build_packet(uint8_t type, const google::protobuf::Message& body);

/// 组一个 user 域响应包
std::vector<char> user_packet(const protocol::user::UserPacket& pkt);

/// 组一个 chat 域响应包
std::vector<char> chat_packet(const protocol::chat::ChatPacket& pkt);

/// 组一个 group 域响应包
std::vector<char> group_packet(const protocol::group::GroupPacket& pkt);

/// user 域 oneof 分发器(user_handler.cpp)
TaskResult on_user_packet(const Task&, const char* body, std::size_t body_len);

/// chat 域 oneof 分发器(chat_handler.cpp)
TaskResult on_chat_packet(const Task&, const char* body, std::size_t body_len);

/// group 域 oneof 分发器(group_handler.cpp)
TaskResult on_group_packet(const Task&, const char* body, std::size_t body_len);

}  // namespace detail
}  // namespace handler
