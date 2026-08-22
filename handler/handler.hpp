#pragma once

#include <cstdint>
#include <string>

#include "../network/thread_pool/thread_pool.hpp"

// ============================================================
//  业务处理模块: 工作线程解析 Task(header+body)、执行业务、返回 TaskResult。
//  职责划分: 主线程收包/发送(server.cpp), 线程池解析/业务/组响应(本模块)。
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
