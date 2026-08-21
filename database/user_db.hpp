#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db.hpp"
#include "protocol/user/user.pb.h"

// ============================================================
//  用户域持久化操作 —— 与 user.proto 对齐
//
//  返回值为 user.proto 的 ErrCode 数值(ERR_SUCCESS=0 表示成功,
//  其它取值见 protocol/user/user.proto 的 ErrCode 枚举)。
//  所有函数都要求传入一个已连接、由调用方借出的 Db。
// ============================================================
namespace db {
namespace user {

// 用户是否存在。
bool user_exists(Db&, uint32_t user_id);

// 注册新用户。成功时 user_id 输出新分配的 id, 返回 ERR_SUCCESS;
// 用户名已存在返回 ERR_USER_EXISTS。
// 注册成功即自动建立与自身的好友关系((user_id, user_id, status=1))。
int register_user(Db&, const std::string& username, const std::string& password,
                  const std::string& nickname, uint32_t& user_id);

// 登录校验。成功时填充 info, 把 online 置 1, 并把离线水位 last_offline_ts
// 推进到当前毫秒时间戳(下一次离线时段从此开始); 用户名/密码错误返回 ERR_INVALID_USER。
// last_offline_ts 输出本次登录前的水位, 供 handler 计算离线消息摘要(与 messages.ts 同单位: 毫秒)。
int login_user(Db&, const std::string& username, const std::string& password,
               protocol::user::UserInfo& info, uint64_t& last_offline_ts);

// 登出: online 置 0, 同时把离线水位推进到当前毫秒时间戳(记录本次离线开始时刻)。
int logout_user(Db&, uint32_t user_id);

// 凭据校验: 用户名+密码是否正确。成功时 user_id 输出对应用户 id;
// 用户名或密码错误返回 ERR_INVALID_USER。无任何副作用。
int verify_user(Db&, const std::string& username, const std::string& password,
                uint32_t& user_id);

// 注销账号: 删除该用户及其关联数据(好友关系/拉黑/聊天消息, 双向)。
// 调用方应先用 verify_user 校验凭据并取得 user_id。
int cancel_user(Db&, uint32_t user_id);

// 发起加好友申请 from → to。
// 自加 ERR_INVALID_PARAM / 目标不存在 ERR_INVALID_USER /
// 已是好友 ERR_ALREADY_FRIEND / 已有待处理申请 ERR_REQUEST_PENDING /
// 任一方向已拉黑 ERR_BLOCKED。
int friend_request(Db&, uint32_t from_id, uint32_t to_id, const std::string& remark);

// 拉取待处理申请列表(别人申请了我, 待我处理)。
int friend_pending_list(Db&, uint32_t user_id,
                        std::vector<protocol::user::FriendPendingItem>& items);

// 删除好友(双向删除), 同时清除双方互拉黑记录(blocks),
// 使删除后再发消息报 ERR_NOT_FRIEND 而非 ERR_BLOCKED。
// 与自身的好友关系不可删除(注册时自动建立), 自引用删除返回 ERR_INVALID_PARAM。
int friend_del(Db&, uint32_t user_id, uint32_t friend_id);

// 好友列表: 返回所有已接受(status=1)好友的 user_id/nickname/online(不含自好友)。
int friend_list(Db&, uint32_t user_id,
                std::vector<protocol::user::FriendListItem>& out);

// 拉黑 / 取消拉黑。只操作独立的 blocks 表, 不改动 friends 好友状态;
// 拉黑仅作消息发送闸门。拉黑(block=true)插入 blocker_id→blockee_id 记录,
// 取消拉黑(block=false)删除该记录。
int friend_block(Db&, uint32_t user_id, uint32_t friend_id, bool block);

// 好友系统规则: "对方申请了我, 我给他发一条私聊即视为接受"。
// 若存在 to → from 的 pending 申请, 由 from 给 to 发私聊时调用本函数,
// 将双方关系置为 accepted。返回是否发生了接受。
bool friend_accept_by_chat(Db&, uint32_t from_id, uint32_t to_id);

// 任一方向已拉黑(blocks 表存在 blocker/blockee 为双方任一侧的记录)。
// 用于聊天发送前拦截(handler::on_chat_send)与加好友前拦截(friend_request)。
bool friend_is_blocked(Db&, uint32_t user_id, uint32_t peer_id);

// 是否互为好友(任一方向存在 status=1 记录)。
// 用于聊天发送/历史拉取前拦截(handler::on_chat_send / on_chat_history)。
bool friend_are_friends(Db&, uint32_t user_id, uint32_t peer_id);

// 获取某用户所有已建立好友关系(status=1, 双向已接受)的好友 user_id 列表。
int friend_ids(Db&, uint32_t user_id, std::vector<uint32_t>& out);

// 获取用户昵称; 用户不存在返回 false。
bool get_nickname(Db&, uint32_t user_id, std::string& out);

}  // namespace user
}  // namespace db
