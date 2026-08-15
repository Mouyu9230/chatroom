#pragma once

#include <cstdint>
#include <string>

#include "../network/thread_pool/thread_pool.hpp"

// ============================================================
//  业务处理模块
//
//  定位: 工作线程拿到 Task(完整请求包: header + body) 后,
//        在这里按消息类型解析 protobuf body、执行业务逻辑,
//        最后返回 TaskResult(响应包 + 目标 fd)。
//
//  职责划分:
//    - 主线程(I/O):  收包/封包/发送  (server.cpp)
//    - 线程池:        解析/业务/组响应 (本文件)
// ============================================================
namespace handler {

/// 处理一个完整请求数据包, 返回响应。
/// task.data 布局: packet_header(8B) + protobuf body
TaskResult handle_task(const Task& task);

/// 构造一条定向系统通知推送 (server -> client)。
/// 参数: uid = 目标用户, content = 通知内容。
/// 返回 PendingPush, 调用方 append 到 result.pushes, 主线程按 uid 在线表转发。
/// 构造方式与 on_chat_send 的 ChatNotify 推送一致(见 handler.cpp on_chat_send)。
PendingPush make_system_notify(uint32_t uid, const std::string& content);

}  // namespace handler
