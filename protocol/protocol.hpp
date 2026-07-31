#pragma once

#include "(!)message/message.pb.h"

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
// ============================================================
#pragma pack(1)
struct packet_header {
    uint16_t magic;
    uint8_t  ver;
    uint8_t  type;     // MsgType 枚举值, 与 message.proto 同步
    uint32_t body_len;
};
#pragma pack()

}  // namespace protocol
