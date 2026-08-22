#include "handler_internal.hpp"

#include "../network/project_path.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>

#include "../database/db_pool.hpp"
#include "../database/user_db.hpp"
#include "network/thread_pool/task_queue.hpp"

// ============================================================
//  file 域 —— 聊天室文件传输(收件箱模型), 仿 FTP STOR/RETR/LIST/REST。
//  复用单条 TLS 连接 + epoll + 线程池, 不另开数据通道。
//  文件按 FILE_CHUNK_SIZE(1MB)分片, 每片一次请求/响应(串行 = 有序 + 流控 + 续传)。
//
//  存储: server/files/<接收者id>_<文件名>, 平铺不建目录; 文件名取 basename 清洗防穿越。
//  会话: 线程池无状态, 用全局表 g_sessions(发送方→目标)串起 SendReq 后的 Chunk/Finish,
//  互斥锁保护; 上传结束即删, 半成品文件保留供续传。
// ============================================================
namespace handler {
namespace {

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// 文件存储根目录: 由可执行文件位置解析到项目根下(server/files/), 目录为仓库内
// 已存在的目录, 代码不创建。与启动时的工作目录无关(避免从 build/ 等非仓库根目录
// 启动时 files 落到别处)。
static std::string file_store_root() {
    static const std::string d = project_root() + "/server/files";
    return d;
}

// 清洗文件名: 只取 basename, 拒绝空/"."/".."/控制字符, 并限制长度。
// 非法输入返回空串。
std::string sanitize_name(const std::string& raw) {
    std::string name = raw;
    auto pos = name.find_last_of('/');
    if (pos != std::string::npos) name = name.substr(pos + 1);
    pos = name.find_last_of('\\');
    if (pos != std::string::npos) name = name.substr(pos + 1);
    if (name.empty() || name == "." || name == "..") return {};
    if (name.size() > 128) name = name.substr(0, 128);
    for (char c : name) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return {};
    }
    return name;
}

// 文件路径: server/files/ 下平铺, 文件名带接收者前缀 "<接收者id>_<name>"。
// 前缀让"仅接收者可下载"天然成立: fget/flist 都按请求者自己的 id 拼路径/过滤,
// 非接收者拼不出正确路径(也不可见于其 flist), 同时不同接收者的同名文件互不冲突。
std::string file_path(uint32_t receiver_id, const std::string& name) {
    return file_store_root() + "/" + std::to_string(receiver_id) + "_" + name;
}

// 上传会话: FileSendReq 协商出的写入目标, 供后续 FileChunk/Finish 使用
struct UploadSession {
    uint32_t    to_id = 0;
    std::string name;
};

std::mutex g_session_mtx;
std::unordered_map<uint32_t, UploadSession> g_sessions;  // 发送方 user_id -> 会话

TaskResult on_file_send_req(const Task& task, const protocol::file::FileSendReq& req) {
    protocol::file::FilePacket resp;
    auto* r = resp.mutable_send_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::file_packet(resp), false};
    }
    std::string name = sanitize_name(req.name());
    if (name.empty() || req.to_id() == 0) {
        r->set_err(protocol::user::ERR_INVALID_PARAM);
        return {task.fd, detail::file_packet(resp), false};
    }
    const uint32_t to_id = req.to_id();

    // 校验次序复用 on_chat_send 的规则: 接收者存在 + 双方为好友
    DbGuard g(db_pool());
    if (!g) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::file_packet(resp), false};
    }
    if (!db::user::user_exists(*g, to_id)) {
        r->set_err(protocol::user::ERR_INVALID_USER);
        return {task.fd, detail::file_packet(resp), false};
    }
    if (!db::user::friend_are_friends(*g, task.user_id, to_id)) {
        r->set_err(protocol::user::ERR_NOT_FRIEND);
        return {task.fd, detail::file_packet(resp), false};
    }

    // 打开(不截断), 取已存字节数作续传起点 —— 语义同 ftp STOR + REST 起点。
    // server/files/ 为仓库内已存在目录, 不再由代码创建。
    int fd = ::open(file_path(to_id, name).c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::file_packet(resp), false};
    }
    off_t size = ::lseek(fd, 0, SEEK_END);
    ::close(fd);

    {
        std::lock_guard<std::mutex> lk(g_session_mtx);
        g_sessions[task.user_id] = {to_id, name};
    }

    r->set_err(protocol::user::ERR_SUCCESS);
    r->set_offset(size > 0 ? static_cast<uint64_t>(size) : 0);
    fprintf(stdout, "[file] user=%u -> %u upload begin: %s (resume offset=%llu)\n",
            task.user_id, to_id, name.c_str(), static_cast<unsigned long long>(size));
    return {task.fd, detail::file_packet(resp), false};
}

TaskResult on_file_chunk(const Task& task, const protocol::file::FileChunk& chunk) {
    protocol::file::FilePacket resp;
    auto* r = resp.mutable_chunk_ack();
    UploadSession sess;
    {
        std::lock_guard<std::mutex> lk(g_session_mtx);
        auto it = g_sessions.find(task.user_id);
        if (it == g_sessions.end()) {
            r->set_err(protocol::user::ERR_INVALID_PARAM);   // 未先 FileSendReq
            return {task.fd, detail::file_packet(resp), false};
        }
        sess = it->second;
    }
    const std::string& data = chunk.data();
    if (data.size() > protocol::FILE_CHUNK_SIZE) {
        r->set_err(protocol::user::ERR_INVALID_PARAM);
        return {task.fd, detail::file_packet(resp), false};
    }

    int fd = ::open(file_path(sess.to_id, sess.name).c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::file_packet(resp), false};
    }
    if (::lseek(fd, static_cast<off_t>(chunk.offset()), SEEK_SET) < 0) {
        ::close(fd);
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::file_packet(resp), false};
    }
    size_t written = 0;
    while (written < data.size()) {
        ssize_t n = ::write(fd, data.data() + written, data.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ::close(fd);
            r->set_err(protocol::user::ERR_SYSTEM);
            return {task.fd, detail::file_packet(resp), false};
        }
        written += static_cast<size_t>(n);
    }
    ::close(fd);
    r->set_err(protocol::user::ERR_SUCCESS);
    r->set_offset(chunk.offset() + data.size());
    return {task.fd, detail::file_packet(resp), false};
}

TaskResult on_file_send_finish(const Task& task, const protocol::file::FileSendFinish& req) {
    (void)req;
    protocol::file::FilePacket resp;
    auto* r = resp.mutable_send_ack();
    UploadSession sess;
    {
        std::lock_guard<std::mutex> lk(g_session_mtx);
        auto it = g_sessions.find(task.user_id);
        if (it == g_sessions.end()) {
            r->set_err(protocol::user::ERR_INVALID_PARAM);   // 未先 FileSendReq
            return {task.fd, detail::file_packet(resp), false};
        }
        sess = it->second;
        g_sessions.erase(it);   // 上传会话到此结束
    }

    struct stat st;
    uint64_t size = 0;
    if (::stat(file_path(sess.to_id, sess.name).c_str(), &st) == 0) {
        size = static_cast<uint64_t>(st.st_size);
    }
    r->set_err(protocol::user::ERR_SUCCESS);
    r->set_size(size);
    fprintf(stdout, "[file] user=%u -> %u upload done: %s (%llu bytes)\n",
            task.user_id, sess.to_id, sess.name.c_str(),
            static_cast<unsigned long long>(size));

    TaskResult result{task.fd, detail::file_packet(resp), false};
    // 向接收方推送 FileNotify(离线则主线程按在线表丢弃, 文件已落盘可 flist)
    protocol::file::FilePacket push;
    auto* n = push.mutable_notify();
    n->set_from_id(task.user_id);
    n->mutable_meta()->set_name(sess.name);
    n->mutable_meta()->set_size(size);
    n->mutable_meta()->set_ts(now_ms());
    result.pushes.push_back({sess.to_id, detail::file_packet(push)});
    return result;
}

TaskResult on_file_get_req(const Task& task, const protocol::file::FileGetReq& req) {
    protocol::file::FilePacket resp;
    auto* r = resp.mutable_get_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::file_packet(resp), false};
    }
    std::string name = sanitize_name(req.name());
    if (name.empty()) {
        r->set_err(protocol::user::ERR_INVALID_PARAM);
        return {task.fd, detail::file_packet(resp), false};
    }

    int fd = ::open(file_path(task.user_id, name).c_str(), O_RDONLY);
    if (fd < 0) {
        r->set_err(protocol::user::ERR_INVALID_PARAM);   // 文件不存在
        return {task.fd, detail::file_packet(resp), false};
    }
    off_t total = ::lseek(fd, 0, SEEK_END);
    const uint64_t offset = req.offset();
    if (offset >= static_cast<uint64_t>(total)) {
        ::close(fd);
        r->set_err(protocol::user::ERR_SUCCESS);
        r->set_total(static_cast<uint64_t>(total));
        r->set_eof(true);   // data 留空
        return {task.fd, detail::file_packet(resp), false};
    }
    if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
        ::close(fd);
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::file_packet(resp), false};
    }
    std::string data;
    data.resize(protocol::FILE_CHUNK_SIZE);
    ssize_t n = ::read(fd, &data[0], protocol::FILE_CHUNK_SIZE);
    ::close(fd);
    if (n <= 0) {
        r->set_err(protocol::user::ERR_SYSTEM);
        return {task.fd, detail::file_packet(resp), false};
    }
    data.resize(static_cast<size_t>(n));
    r->set_err(protocol::user::ERR_SUCCESS);
    r->set_data(data);
    r->set_offset(offset);
    r->set_total(static_cast<uint64_t>(total));
    r->set_eof(offset + static_cast<uint64_t>(n) >= static_cast<uint64_t>(total));
    return {task.fd, detail::file_packet(resp), false};
}

TaskResult on_file_list_req(const Task& task, const protocol::file::FileListRequest& req) {
    protocol::file::FilePacket resp;
    auto* r = resp.mutable_list_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::file_packet(resp), false};
    }
    // 平铺存储 + 接收者前缀: 只列出"发给我的"文件(文件名以 "<我的id>_" 开头),
    // 展示时去掉前缀, 非接收者的文件不可见。
    const std::string dir = file_store_root();
    const std::string prefix = std::to_string(task.user_id) + "_";

    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
        r->set_err(protocol::user::ERR_SUCCESS);   // 目录不可用 = 空列表
        return {task.fd, detail::file_packet(resp), false};
    }
    struct dirent* ent;
    while ((ent = ::readdir(d)) != nullptr) {
        std::string fname = ent->d_name;
        if (fname == "." || fname == "..") continue;
        if (fname.compare(0, prefix.size(), prefix) != 0) continue;   // 非本接收者的跳过
        std::string path = dir + "/" + fname;
        struct stat st;
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        auto* m = r->mutable_files()->Add();
        m->set_name(fname.substr(prefix.size()));   // 去掉 "<id>_" 前缀后展示原名
        m->set_size(static_cast<uint64_t>(st.st_size));
        m->set_ts(static_cast<uint64_t>(st.st_mtime) * 1000);
    }
    ::closedir(d);
    r->set_err(protocol::user::ERR_SUCCESS);
    return {task.fd, detail::file_packet(resp), false};
}

TaskResult on_file_stat_req(const Task& task, const protocol::file::FileStatRequest& req) {
    protocol::file::FilePacket resp;
    auto* r = resp.mutable_stat_resp();
    if (task.user_id == 0) {
        r->set_err(protocol::user::ERR_NOT_LOGGED_IN);
        return {task.fd, detail::file_packet(resp), false};
    }
    std::string name = sanitize_name(req.name());
    if (name.empty()) {
        r->set_err(protocol::user::ERR_INVALID_PARAM);
        return {task.fd, detail::file_packet(resp), false};
    }
    struct stat st;
    if (::stat(file_path(task.user_id, name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        r->set_err(protocol::user::ERR_INVALID_PARAM);
        return {task.fd, detail::file_packet(resp), false};
    }
    r->set_err(protocol::user::ERR_SUCCESS);
    r->set_size(static_cast<uint64_t>(st.st_size));
    return {task.fd, detail::file_packet(resp), false};
}

}  // namespace

namespace detail {

TaskResult on_file_packet(const Task& task, const char* body, size_t body_len) {
    protocol::file::FilePacket pkt;
    if (!pkt.ParseFromArray(body, static_cast<int>(body_len))) {
        fprintf(stderr, "[handler] bad file packet body\n");
        return {task.fd, {}, true};
    }
    switch (pkt.body_case()) {
        case protocol::file::FilePacket::kListReq:
            return on_file_list_req(task, pkt.list_req());
        case protocol::file::FilePacket::kStatReq:
            return on_file_stat_req(task, pkt.stat_req());
        case protocol::file::FilePacket::kSendReq:
            return on_file_send_req(task, pkt.send_req());
        case protocol::file::FilePacket::kChunk:
            return on_file_chunk(task, pkt.chunk());
        case protocol::file::FilePacket::kSendFinish:
            return on_file_send_finish(task, pkt.send_finish());
        case protocol::file::FilePacket::kGetReq:
            return on_file_get_req(task, pkt.get_req());
        default:
            fprintf(stderr, "[handler] unknown file packet case: %d\n",
                    static_cast<int>(pkt.body_case()));
            return {task.fd, {}, true};
    }
}

}  // namespace detail

}  // namespace handler
