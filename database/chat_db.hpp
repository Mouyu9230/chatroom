#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db.hpp"
#include "protocol/chat/chat.pb.h"

// ============================================================
//  聊天域持久化操作 —— 与 chat.proto 对齐
//
//  返回值为 user.proto 的 ErrCode 数值(ERR_SUCCESS=0 表示成功)。
// ============================================================
namespace db {
namespace chat {

// 保存一条消息。msg_id/ts 输出服务端分配/生成的值。
int save_message(Db&, uint32_t from_id, uint32_t to_id, uint32_t to_type,
                 const std::string& content, uint64_t& msg_id, uint64_t& ts);

// 拉取与 target 的会话历史(双向), 按 msg_id 升序, 最多 limit 条(0<limit<=200)。
int query_history(Db&, uint32_t self_id, uint32_t target_id, uint64_t after_msg_id,
                  uint32_t limit, std::vector<protocol::chat::ChatMessage>& out);

}  // namespace chat
}  // namespace db
