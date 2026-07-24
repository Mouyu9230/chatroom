#include "thread_pool.hpp"


ThreadPool::ThreadPool(size_t thread_count):workers_(thread_count){}



ThreadPool::~ThreadPool() {
    stop();
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
        }
    }
}
