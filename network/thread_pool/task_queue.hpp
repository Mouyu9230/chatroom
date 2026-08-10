#pragma once

#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>


//   主线程（单 IO epoll）               线程池
//   ──────────────────                ──────────
//   epoll_wait                        Worker 1
//     ├─ Recv → FetchPacket            Worker 2
//     ├─ 封装成 Task ────► TaskQueue ──► Worker 3
//     │                                 处理业务逻辑
//     │     ◄── ResultQueue ◄───────────┘
//     │     TaskResult（响应+目标fd）
//     │
//     ├─ AppendSendBuffer → mod EPOLLOUT
//     └─ Send



// 携带一个完整数据包，线程池解析后处理业务逻辑
struct Task {
    int fd = -1;                // 来源连接 fd
    uint32_t user_id = 0;       // 来源用户 ID（0=未登录）
    std::vector<char> data;     // 完整的请求数据包（header + body）
};


//线程池处理完业务后，返回响应数据和目标fd
struct TaskResult {
    int fd = -1;                // 响应发往哪个 fd（-1 表示无操作）
    std::vector<char> data;     // 完整的响应数据包（header + body）
    bool need_close = false;    // true = 主线程需关闭此连接
    uint32_t user_id = 0;       // 非 0 时, 主线程把该连接绑定为这个用户(登录成功)
};

 
template<typename T>
class MpscQueue {
public:

    MpscQueue() = default;

    ~MpscQueue() { close(); }

    void push(T item){
        {
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push(std::move(item));
        } 
        cv_.notify_one();
    }

    /// 非阻塞出队
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.empty()) 
            return false;
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    } 

    /// 阻塞出队
    bool wait_and_pop(T& out) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) 
            return false;
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    /// 关闭队列，唤醒所有阻塞的等待者
    void close() {
        {
            std::lock_guard<std::mutex> lock(   mtx_); 
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

private:

    mutable std::mutex mtx_;//mutable以在 const 函数中加锁
    std::queue<T> queue_;
    std::condition_variable cv_;  
    bool closed_ = false;

};
