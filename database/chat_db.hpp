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

// 拉取与 target 的会话历史(双向, 仅单聊 to_type=1), 按 msg_id 降序(从新到旧),
// 最多 limit 条(0<limit<=200)。
int query_history(Db&, uint32_t self_id, uint32_t target_id, uint64_t after_msg_id,
                  uint32_t limit, std::vector<protocol::chat::ChatMessage>& out);

// 拉取群历史(to_type=2, to_id=group_id), 按 msg_id 降序, 最多 limit 条。
int query_group_history(Db&, uint32_t group_id, uint64_t after_msg_id,
                        uint32_t limit, std::vector<protocol::chat::ChatMessage>& out);

// 离线消息摘要项: 发送者 + 昵称 + 条数。
struct OfflineItem {
    uint32_t from_id;
    std::string nickname;
    uint32_t count;
};

// 统计 ts > after_ts 期间发给 self 的 1:1 消息, 按发送者分组计数
// (排除发给自己的消息)。供用户上线时生成"离线时段收到消息"通知。
int offline_summary(Db&, uint32_t self_id, uint64_t after_ts,
                    std::vector<OfflineItem>& out);

}  // namespace chat
}  // namespace db
