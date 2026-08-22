#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../protocol/protocol.hpp"
#include "../protocol/user/user.pb.h"
#include "../protocol/chat/chat.pb.h"
#include "../protocol/group/group.pb.h"
#include "../protocol/file/file.pb.h"
#include "handler.hpp"

// ============================================================
//  handler 内部共享工具 —— 仅供 handler 各域实现文件使用。
//  暴露 handler 域间共享的接口:
//    build_packet / *_packet: 组包工具(handler.cpp)
//    on_user_packet / on_chat_packet / on_group_packet: 各域 oneof 分发器
//    (user/chat/group_handler.cpp), 由 handle_task 按 hdr->type 调用。
//  不对外暴露。
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

/// 组一个 file 域响应包
std::vector<char> file_packet(const protocol::file::FilePacket& pkt);

/// user 域 oneof 分发器(user_handler.cpp)
TaskResult on_user_packet(const Task&, const char* body, std::size_t body_len);

/// chat 域 oneof 分发器(chat_handler.cpp)
TaskResult on_chat_packet(const Task&, const char* body, std::size_t body_len);

/// group 域 oneof 分发器(group_handler.cpp)
TaskResult on_group_packet(const Task&, const char* body, std::size_t body_len);

/// file 域 oneof 分发器(file_handler.cpp)
TaskResult on_file_packet(const Task&, const char* body, std::size_t body_len);

}  // namespace detail
}  // namespace handler
