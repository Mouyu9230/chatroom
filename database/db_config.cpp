#include "db_config.hpp"

#include <cstdlib>

void DbConfig::load_from_env() {
    if (const char* v = std::getenv("CHATROOM_DB_HOST")) host = v;
    if (const char* v = std::getenv("CHATROOM_DB_PORT")) port = static_cast<uint16_t>(std::atoi(v));
    if (const char* v = std::getenv("CHATROOM_DB_USER")) user = v;
    if (const char* v = std::getenv("CHATROOM_DB_PASS")) password = v;
    if (const char* v = std::getenv("CHATROOM_DB_NAME")) database = v;
    if (const char* v = std::getenv("CHATROOM_DB_POOL")) {
        int n = std::atoi(v);
        pool_size = (n > 0) ? static_cast<std::size_t>(n) : 1;
    }
}
