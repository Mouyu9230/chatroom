#pragma once

#include <stdint.h>

// ── 协议版本 ──
const int PROTOCOL_VERSION = 1;

// ── 消息体最大长度（16MB，防止单次发送撑爆内存） ──
const uint32_t MAX_BODY_LEN = 16 * 1024 * 1024;

// ── 消息类型 ──
enum MsgType {
    MSG_LOGIN_REQ      = 0x01,  // C→S  登录请求
    MSG_LOGIN_RESP     = 0x02,  // S→C  登录响应
    MSG_REGISTER_REQ   = 0x03,  // C→S  注册请求
    MSG_REGISTER_RESP  = 0x04,  // S→C  注册响应
    MSG_CHAT_REQ       = 0x05,  // C→S  发送聊天消息
    MSG_CHAT_RESP      = 0x06,  // S→C  聊天消息（单聊 / 群发）
    MSG_LOGOUT_REQ     = 0x07,  // C→S  退出请求
    MSG_LOGOUT_RESP    = 0x08,  // S→C  退出响应
    MSG_HEARTBEAT_REQ  = 0x09,  // C→S  ping
    MSG_HEARTBEAT_RESP = 0x0A,  // S→C  pong
};

// ── 错误码（用于登录 / 注册等响应） ──
enum ErrCode {
    ERR_SUCCESS        = 0,  // 成功
    ERR_SYSTEM         = 1,  // 系统内部错误
    ERR_INVALID_PARAM  = 2,  // 参数错误
    ERR_INVALID_USER   = 3,  // 用户名或密码错误
    ERR_USER_EXISTS    = 4,  // 用户已存在
    ERR_NOT_LOGGED_IN  = 5,  // 未登录
    ERR_FULL           = 6,  // 服务器已满
};

// ── 消息头 ──
// 所有消息均以固定 8 字节头部开始，之后紧跟 body（可为空）
// 格式： magic(2) + version(1) + type(1) + body_len(4)
#pragma pack(1)
struct MsgHeader {
    uint16_t magic;     // 魔数，固定 0xCAFE，用于校验
    uint8_t  version;   // 协议版本号
    uint8_t  type;      // 消息类型（MsgType）
    uint32_t body_len;  // body 长度（不含头部本身）
};
#pragma pack()

const uint16_t MAGIC_NUMBER = 0xCAFE;
const int      HEADER_LEN   = sizeof(MsgHeader);

// ── body 结构体定义 ──
// 以下结构体都是 body 部分的负载格式，前面统一带上 MsgHeader

// 登录请求 body
struct BodyLoginReq {
    char username[64];
    char password[64];
};

// 登录响应 / 注册响应 body
struct BodyLoginResp {
    uint8_t  err;       // ErrCode
    uint32_t userid;    // 登录成功后分配的 userid（失败时 = 0）
};

// 注册请求 body
struct BodyRegisterReq {
    char username[64];
    char password[64];
    char nickname[64];
};

// 聊天请求 body（C→S）
// 固定部分 + 变长 content，content 起始地址 = body + sizeof(BodyChatReq)
struct BodyChatReq {
    uint32_t receiver_id;  // 目标用户 userid（0 表示群发）
};

// 聊天消息 body（S→C，转发给接收方）
struct BodyChatResp {
    uint32_t sender_id;      // 发送者 userid
    char     sender_name[64];
};

// 退出登录 body（C→S / S→C 共用）
struct BodyLogout {
    uint32_t userid;
};

// 心跳 body（无额外字段）
// 仅头部即可，body_len = 0
