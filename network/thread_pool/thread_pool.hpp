#pragma once

#include "task_queue.hpp"

#include <functional>
#include <thread>
#include <vector>


class ThreadPool {
public:
    using TaskHandler = std::function<TaskResult(Task)>;

    explicit ThreadPool(size_t thread_count = std::thread::hardware_concurrency());//获取当前系统支持的并发线程数
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// 启动 thread_count_ 个工作线程，每个线程反复执行 handler
    void start(TaskHandler handler);

    /// 停止所有工作线程：
    ///   1. 关闭任务队列，唤醒所有阻塞的 worker
    ///   2. worker 排空队列中的剩余任务
    ///   3. join 所有线程
    void stop();

    /// 投递一个任务到线程池
    void submit(Task task);

    /// 非阻塞地取回一个处理结果，返回 false 表示暂无结果
    bool try_get_result(TaskResult& out);

    /// 直接访问底层队列（用于特殊场景，如注入 poison pill）
    MpscQueue<Task>&       task_queue()       { return task_queue_; }
    MpscQueue<TaskResult>& result_queue()      { return result_queue_; }

    /// 结果就绪通知事件描述符(eventfd, 可注册到 epoll)。
    /// 每次 result_queue_ 入队后写入一次; 主线程读到可读事件后应排空计数器
    /// 并调用 try_get_result 循环取回所有结果。创建失败返回 -1(降级为轮询)。
    int result_event_fd() const { return result_event_fd_; }

private:
    void worker_loop(TaskHandler handler);
    void notify_result();   // 向 eventfd 写 1, 唤醒等待结果的 epoll 循环

    std::vector<std::thread> workers_;
    MpscQueue<Task>          task_queue_;
    MpscQueue<TaskResult>    result_queue_;
    int result_event_fd_ = -1;   // 结果就绪通知; 创建失败为 -1
};
