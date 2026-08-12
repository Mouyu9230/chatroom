-- ============================================================
--  chatroom 数据库结构 (参考; db_init.cpp 内嵌了同款 DDL, 服务端
--  启动会自动 CREATE TABLE IF NOT EXISTS, 一般无需手动执行本文件)
--
--  status 含义 (friends 表):
--    0 = pending 待处理申请
--    1 = accepted 已建立好友关系
--    2 = blocked 已拉黑
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

CREATE TABLE IF NOT EXISTS messages (
  msg_id  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  from_id INT UNSIGNED NOT NULL,
  to_id   INT UNSIGNED NOT NULL,
  to_type TINYINT      NOT NULL DEFAULT 1,
  content TEXT         NOT NULL,
  ts      BIGINT UNSIGNED NOT NULL,
  KEY idx_to (to_id, msg_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
