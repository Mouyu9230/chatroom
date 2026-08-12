#pragma once

#include <cstdint>
#include <string>

// ============================================================
//  MySQL 连接配置
//
//  默认值 + 环境变量覆盖 (CHATROOM_DB_*):
//    CHATROOM_DB_HOST  默认 127.0.0.1
//    CHATROOM_DB_PORT  默认 3306
//    CHATROOM_DB_USER  默认 chatroom (对应建库指令创建的账号)
//    CHATROOM_DB_PASS  默认 chatroom
//    CHATROOM_DB_NAME  默认 chatroom
//    CHATROOM_DB_POOL  默认 4
// ============================================================
struct DbConfig {
    std::string host      = "127.0.0.1";
    uint16_t    port      = 3306;
    std::string user      = "chatroom";
    std::string password  = "chatroom";
    std::string database  = "chatroom";
    std::size_t pool_size = 4;

    void load_from_env();
};
