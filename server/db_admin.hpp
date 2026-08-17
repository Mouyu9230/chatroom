#pragma once

// ============================================================
//  db_admin —— 运行期数据库查看控制台 (server 侧调试工具)
//
//  用途: 服务运行中从 stdin 读取命令, 实时查看 MySQL 底层数据
//        (users / friends / blocks / messages 等表), 便于排查业务逻辑。
//  只读 SELECT, 不涉及协议/客户端改动, 不修改任何数据。
//
//  使用: server 进程运行时在终端输入命令 (见 help), 读 stdin 的
//        detached 线程; stdin 为 EOF(/dev/null/后台运行) 时线程自行结束。
// ============================================================
namespace dbadmin {

// 启动 stdin 调试控制台线程(detached, 读到 EOF 或输入 quit/exit 时结束)。
void start_console();

}  // namespace dbadmin
