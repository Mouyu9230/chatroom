#pragma once

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

}  // namespace handler
