#pragma once

#include <cstdint>

namespace protocol {


const uint16_t MAGIC       = 0xCAFE;  // 魔数，用于校验
const uint8_t  VERSION     = 1;       // 协议版本

const uint32_t HEADER_LEN  = 8;       // 固定头部长度
const uint32_t MAX_BODY_LEN = 16 * 1024 * 1024;  // body 最大 16MB



enum msg_type : uint8_t {
    TYPE_LOGIN_REQ      = 0x01,  //登录请求
    TYPE_LOGIN_RESP     = 0x02,  //登录响应
    TYPE_REGISTER_REQ   = 0x03,  //注册请求
    TYPE_REGISTER_RESP  = 0x04,  //注册响应
    TYPE_CANCEL_REQ     = 0x05,  //注销请求
    TYPE_CANCEL_RESP    = 0x06,  //注销响应
    TYPE_LOGOUT_REQ     = 0x07,  //退出请求
    TYPE_LOGOUT_RESP    = 0x08,  //退出响应
    TYPE_HEARTBEAT_REQ  = 0x09,  //ping
    TYPE_HEARTBEAT_RESP = 0x0A,  //pong
    TYPE_SYSTEM_NOTIFY  = 0x0B,  //系统通知
    TYPE_NULL           = 0x0C,
    TYPE_NULL           = 0x0D,
    TYPE_NULL           = 0x0E,
    TYPE_NULL           = 0x0F,
    TYPE_NULL           = 0x11,
};



enum err_code : uint8_t {
    ERR_SUCCESS       = 0,  // 成功
    ERR_SYSTEM        = 1,  // 系统内部错误
    ERR_INVALID_PARAM = 2,  // 参数错误
    ERR_INVALID_USER  = 3,  // 用户名或密码错误
    ERR_USER_EXISTS   = 4,  // 用户已存在
    ERR_NOT_LOGGED_IN = 5,  // 未登录
    ERR_FULL          = 6,  // 服务器已满
};


//消息头
//固定8字节
//布局： magic(2) + version(1) + type(1) + body_len(4)

#pragma pack(1)
struct packet_header {
    uint16_t magic;      // 魔数 MAGIC
    uint8_t  version;    // 协议版本 VERSION
    uint8_t  type;       // 消息类型（msg_type）
    uint32_t body_len;   // body 长度（不含头部本身）
};
#pragma pack()




struct login_request {
    char username[64];
    char password[64];
};

struct login_response {
    uint8_t  err;      
    uint32_t userid;     
};

struct register_request {
    char username[64];
    char password[64];
    char nickname[64];
};

struct register_response {
    uint8_t  err;       
    uint32_t userid;    
};

// 聊天请求body，固定部分+变长content
// content起始地址=body+sizeof(chat_request)
struct chat_request {
    uint32_t receiver_id;  
};



struct logout_body {
    uint32_t userid;
};


} // namespace protocol
