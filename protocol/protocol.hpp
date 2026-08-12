#pragma once

#include <cstdint>

namespace protocol {

// ============================================================
//  协议层常量
// ============================================================
const uint32_t HEADER_LEN    = 8;
const uint32_t MAX_BODY_LEN  = 16 * 1024 * 1024;  // body 最大 16MB
const uint32_t MAX_NICK_LEN  = 64;
const uint32_t MAX_MSG_LEN   = 4096;  // 单条消息最大长度

// 魔数，用于校验协议合法性
const uint16_t MAGIC_NUM = 0x9230;

// ============================================================
//  消息头，固定 8 字节，布局: magic(2) + version(1) + type(1) + body_len(4)
//
//  type 字段自 v2 起表示"域"(domain), 不再承载扁平消息类型:
//    DOMAIN_USER = 1  → body 为 protocol.user.UserPacket(oneof)
//    DOMAIN_CHAT = 2  → body 为 protocol.chat.ChatPacket(oneof)
//  具体消息由信封的 oneof 分支区分, 见 user.proto / chat.proto。
// ============================================================
enum Domain : uint8_t {
    DOMAIN_USER = 1,
    DOMAIN_CHAT = 2,
};

#pragma pack(1)
struct packet_header {
    uint16_t magic;
    uint8_t  ver;
    uint8_t  type;     // Domain 枚举值
    uint32_t body_len;
};
#pragma pack()

}  // namespace protocol
