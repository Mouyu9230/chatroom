#include "db_init.hpp"

namespace {

// 与 schema.sql 一致的 DDL。字段与 user.proto / chat.proto / group.proto 实体对齐:
//   users    —— UserInfo / Register / Login
//   friends  —— 好友系统(FriendRequest/Pending/Del/Check/Block)
//   messages —— ChatMessage(ChatSend / History)
//   group_info / group_members / group_applications —— 群域(群主/管理员/申请审批)
const char* const kDDL[] = {
    "CREATE TABLE IF NOT EXISTS users ("
    "  user_id    INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
    "  username   VARCHAR(64)  NOT NULL,"
    "  password   VARCHAR(64)  NOT NULL,"
    "  nickname   VARCHAR(64)  NOT NULL DEFAULT '',"
    "  created_at BIGINT UNSIGNED NOT NULL,"
    "  online     TINYINT      NOT NULL DEFAULT 0,"
    "  last_offline_ts BIGINT UNSIGNED NOT NULL DEFAULT 0,"
    "  UNIQUE KEY uk_username (username)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

    "CREATE TABLE IF NOT EXISTS friends ("
    "  id        INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
    "  user_id   INT UNSIGNED NOT NULL,"
    "  friend_id INT UNSIGNED NOT NULL,"
    "  status    TINYINT      NOT NULL DEFAULT 0,"
    "  remark    VARCHAR(128) NOT NULL DEFAULT '',"
    "  ts        BIGINT UNSIGNED NOT NULL,"
    "  UNIQUE KEY uk_pair (user_id, friend_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

    "CREATE TABLE IF NOT EXISTS blocks ("
    "  id         INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
    "  blocker_id INT UNSIGNED NOT NULL,"
    "  blockee_id INT UNSIGNED NOT NULL,"
    "  ts         BIGINT UNSIGNED NOT NULL,"
    "  UNIQUE KEY uk_pair (blocker_id, blockee_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

    "CREATE TABLE IF NOT EXISTS messages ("
    "  msg_id  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
    "  from_id INT UNSIGNED NOT NULL,"
    "  to_id   INT UNSIGNED NOT NULL,"
    "  to_type TINYINT      NOT NULL DEFAULT 1,"
    "  content TEXT         NOT NULL,"
    "  ts      BIGINT UNSIGNED NOT NULL,"
    "  KEY idx_to (to_id, msg_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

    "CREATE TABLE IF NOT EXISTS group_info ("
    "  group_id   INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
    "  name       VARCHAR(64) NOT NULL,"
    "  owner_id   INT UNSIGNED NOT NULL,"
    "  created_at BIGINT UNSIGNED NOT NULL"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

    "CREATE TABLE IF NOT EXISTS group_members ("
    "  id       INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
    "  group_id INT UNSIGNED NOT NULL,"
    "  user_id  INT UNSIGNED NOT NULL,"
    "  role     TINYINT NOT NULL DEFAULT 3,"
    "  UNIQUE KEY uk_group_user (group_id, user_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

    "CREATE TABLE IF NOT EXISTS group_applications ("
    "  id       INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
    "  group_id INT UNSIGNED NOT NULL,"
    "  user_id  INT UNSIGNED NOT NULL,"
    "  remark   VARCHAR(128) NOT NULL DEFAULT '',"
    "  ts       BIGINT UNSIGNED NOT NULL,"
    "  UNIQUE KEY uk_group_user (group_id, user_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
};

}  // namespace

bool init_db(Db& db, std::string* err) {
    for (const char* sql : kDDL) {
        if (!db.execute(sql)) {
            if (err) *err = db.error();
            return false;
        }
    }
    return true;
}
