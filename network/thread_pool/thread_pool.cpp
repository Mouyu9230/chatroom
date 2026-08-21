#include "thread_pool.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cstdint>

ThreadPool::ThreadPool(size_t thread_count)
    : workers_(thread_count)
    , result_event_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    // 创建失败(资源不足)时 result_event_fd_ = -1, 主循环降级为靠 epoll 超时轮询结果。
}



ThreadPool::~ThreadPool() {
    stop();
    if (result_event_fd_ >= 0) {
        ::close(result_event_fd_);
        result_event_fd_ = -1;
    }
}


void ThreadPool::start(TaskHandler handler) {
    for (auto& t : workers_) {
        t = std::thread(&ThreadPool::worker_loop, this, handler);
    }
}

void ThreadPool::stop() {
    // 1. 关闭任务队列 —— worker 在 wait_and_pop 中被唤醒，排空后退出
    task_queue_.close(); 

    // 2. 等待所有 worker 退出
    for (auto& t : workers_) {
        if (t.joinable())
            t.join();
    }
}


void ThreadPool::submit(Task task) {
    task_queue_.push(std::move(task));
}

bool ThreadPool::try_get_result(TaskResult& out) {
    return result_queue_.try_pop(out);
}



void ThreadPool::worker_loop(TaskHandler handler) {

    while (true) {
        Task task;

        if (!task_queue_.wait_and_pop(task))
            break;


        TaskResult result=handler(std::move(task));


        if (result.fd >= 0) {
            result_queue_.push(std::move(result));
            notify_result();   // 唤醒主循环, 立即取回结果(无需等 epoll 超时)
        }
    }
}

void ThreadPool::notify_result() {
    if (result_event_fd_ < 0) return;
    const uint64_t one = 1;
    // 写失败(如 fd 已关)可忽略: 主循环仍会靠 epoll 超时轮询兜底
    ssize_t n = ::write(result_event_fd_, &one, sizeof(one));
    (void)n;
}
