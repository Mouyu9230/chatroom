-- ============================================================
--  chatroom 数据库结构 (参考; db_init.cpp 内嵌了同款 DDL, 服务端
--  启动会自动 CREATE TABLE IF NOT EXISTS, 一般无需手动执行本文件)
--
--  status 含义 (friends 表):
--    0 = pending 待处理申请
--    1 = accepted 已建立好友关系
--  注册用户自动插入一条自身好友关系 (user_id, user_id, status=1),
--  该行不可通过 friend_del 删除(见 user_db.cpp friend_del 自引用拦截)。
--  拉黑独立存于 blocks 表(blocker_id → blockee_id), 不改动 friends 好友状态,
--  仅用于判别消息能否发送(见 friend_is_blocked)。
-- ============================================================

CREATE DATABASE IF NOT EXISTS chatroom CHARACTER SET utf8mb4;

USE chatroom;

CREATE TABLE IF NOT EXISTS users (
  user_id    INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  username   VARCHAR(64)  NOT NULL,
  password   VARCHAR(64)  NOT NULL,
  nickname   VARCHAR(64)  NOT NULL DEFAULT '',
  created_at BIGINT UNSIGNED NOT NULL,
  online     TINYINT      NOT NULL DEFAULT 0,
  last_offline_ts BIGINT UNSIGNED NOT NULL DEFAULT 0,  -- 毫秒; 上次离线水位, 用于上线时离线消息摘要
  UNIQUE KEY uk_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS friends (
  id        INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  user_id   INT UNSIGNED NOT NULL,
  friend_id INT UNSIGNED NOT NULL,
  status    TINYINT      NOT NULL DEFAULT 0,
  remark    VARCHAR(128) NOT NULL DEFAULT '',
  ts        BIGINT UNSIGNED NOT NULL,
  UNIQUE KEY uk_pair (user_id, friend_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS blocks (
  id         INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  blocker_id INT UNSIGNED NOT NULL,
  blockee_id INT UNSIGNED NOT NULL,
  ts         BIGINT UNSIGNED NOT NULL,
  UNIQUE KEY uk_pair (blocker_id, blockee_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS messages (
  msg_id  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  from_id INT UNSIGNED NOT NULL,
  to_id   INT UNSIGNED NOT NULL,
  to_type TINYINT      NOT NULL DEFAULT 1,   -- 1=单聊 2=群聊(to_id 为 group_id)
  content TEXT         NOT NULL,
  ts      BIGINT UNSIGNED NOT NULL,
  KEY idx_to (to_id, msg_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 群组表(to_id 与 users.user_id 在同一命名空间, 查询历史须带 to_type 区分)
CREATE TABLE IF NOT EXISTS group_info (
  group_id   INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  name       VARCHAR(64) NOT NULL,
  owner_id   INT UNSIGNED NOT NULL,
  created_at BIGINT UNSIGNED NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 群成员(仅已入群成员; 申请态在 group_applications)。role: 1=群主 2=管理员 3=普通成员
CREATE TABLE IF NOT EXISTS group_members (
  id       INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  group_id INT UNSIGNED NOT NULL,
  user_id  INT UNSIGNED NOT NULL,
  role     TINYINT NOT NULL DEFAULT 3,
  UNIQUE KEY uk_group_user (group_id, user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 加群申请(待审批)
CREATE TABLE IF NOT EXISTS group_applications (
  id       INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  group_id INT UNSIGNED NOT NULL,
  user_id  INT UNSIGNED NOT NULL,
  remark   VARCHAR(128) NOT NULL DEFAULT '',
  ts       BIGINT UNSIGNED NOT NULL,
  UNIQUE KEY uk_group_user (group_id, user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
