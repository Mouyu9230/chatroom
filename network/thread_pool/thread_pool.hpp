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

private:
    void worker_loop(TaskHandler handler);

    std::vector<std::thread> workers_;
    MpscQueue<Task>          task_queue_;
    MpscQueue<TaskResult>    result_queue_;
};
