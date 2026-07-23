#pragma once

#include <cstdint>

namespace protocol {

const uint32_t HEADER_LEN    = 8;
const uint32_t MAX_BODY_LEN  = 16 * 1024 * 1024;  // body 最大 16MB
const uint32_t MAX_NICK_LEN  = 64;
const uint32_t MAX_MSG_LEN   = 4096;  // 单条消息最大长度

// 消息类型
enum msg_type : uint8_t {
    TYPE_LOGIN_REQ       = 0x01,
    TYPE_LOGIN_RESP      = 0x02,
    TYPE_REGISTER_REQ    = 0x03,
    TYPE_REGISTER_RESP   = 0x04,
    TYPE_CANCEL_REQ      = 0x05,
    TYPE_CANCEL_RESP     = 0x06,
    TYPE_LOGOUT_REQ      = 0x07,
    TYPE_LOGOUT_RESP     = 0x08,
    TYPE_HEARTBEAT_REQ   = 0x09,
    TYPE_HEARTBEAT_RESP  = 0x0A,
    TYPE_SYSTEM_NOTIFY   = 0x0B,
    TYPE_FRIEND_ADD_REQ  = 0x0C,
    TYPE_FRIEND_ADD_RESP = 0x0D,
    TYPE_FRIEND_DEL_REQ  = 0x0E,
    TYPE_FRIEND_DEL_RESP = 0x0F,
    TYPE_FRIEND_CHECK_REQ  = 0x11,
    TYPE_FRIEND_CHECK_RESP = 0x12,
    TYPE_FRIEND_BLOCK_REQ  = 0x13,
    TYPE_FRIEND_BLOCK_RESP = 0x14,
    TYPE_CHAT_REQ        = 0x15,
    TYPE_CHAT_RESP       = 0x16,
};

// 错误码
enum err_code : uint8_t {
    ERR_SUCCESS       = 0,
    ERR_SYSTEM        = 1,
    ERR_INVALID_PARAM = 2,
    ERR_INVALID_USER  = 3,
    ERR_USER_EXISTS   = 4,
    ERR_NOT_LOGGED_IN = 5,
    ERR_FULL          = 6,
    ERR_NOT_FRIEND    = 7,
    ERR_ALREADY_FRIEND= 8,
    ERR_BLOCKED       = 9,
};

// 魔数，用于校验协议合法性
const uint16_t MAGIC_NUM = 0x9230;

// 消息头，固定 8 字节，布局: magic(2) + version(1) + type(1) + body_len(4)
#pragma pack(1)
struct packet_header {
    uint16_t magic;
    uint8_t  ver;
    uint8_t  type;
    uint32_t body_len;
};
#pragma pack()

// 登录
struct login_request {
    char username[64];
    char password[64];
};

struct login_response {
    uint8_t  err;
    uint32_t userid;
};

// 注册
struct register_request {
    char username[64];
    char password[64];
    char nickname[64];
};

struct register_response {
    uint8_t  err;
    uint32_t userid;
};

// 注销
struct cancel_request {
    char username[64];
    char password[64];
};

struct cancel_response {
    uint8_t  err;
    uint32_t userid;
};

// 退出登录
struct logout_request {
    uint32_t userid;
};

struct logout_response {
    uint8_t err;
};

// 心跳: heartbeat_req / heartbeat_resp 均无 body

// 系统通知
struct system_notify {
    char content[256];
};

// 好友管理
struct friend_add_request {
    uint32_t friend_id;
};

struct friend_add_response {
    uint8_t  err;
    uint32_t friend_id;
    char     nickname[64];
};

struct friend_del_request {
    uint32_t friend_id;
};

struct friend_del_response {
    uint8_t err;
};

struct friend_check_request {
    uint32_t friend_id;
};

struct friend_check_response {
    uint8_t err;
    uint8_t is_friend;   // 0=不是, 1=是
    uint8_t is_blocked;  // 0=未屏蔽, 1=已屏蔽
};

struct friend_block_request {
    uint32_t friend_id;
    uint8_t  block;      // 0=解除, 1=开启
};

struct friend_block_response {
    uint8_t err;
};




// 聊天消息，变长 content 紧跟固定头之后
// content 起始地址 = body + sizeof(chat_request)
struct chat_request {
    uint32_t receiver_id;
};

struct chat_response {
    uint8_t  err;
    uint32_t msg_id;
};





}  // namespace protocol
