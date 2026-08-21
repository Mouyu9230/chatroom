#include "client.hpp"

#include "../network/socket/socket.hpp"
#include "../network/project_path.hpp"
#include "../protocol/group/group.pb.h"

#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <mutex>
#include <queue>
#include <thread>

// ------------------------------------------------------------
//  TLS 全局状态(客户端单连接, 全局即可)
//    SSL 对象不支持多线程并发调用(即使方向不同), 因此所有 SSL 读写都必须持锁,
//    且等待期间不能持锁 —— 握手完成后连接转非阻塞, 由 poll 等待事件。
// ------------------------------------------------------------
SSL* g_ssl = nullptr;
std::mutex g_ssl_mtx;

// ============================================================
//  客户端实现
//
//  用法: client [port] [ip]   (默认 127.0.0.1:2100)
//
//  结构: 后台收包线程常驻读 socket, 实时打印服务端推送
//  (ChatNotify / SystemNotify / UserStatusNotify); 请求响应入队,
//  主线程命令循环通过 user_request/chat_request 从队列取回。
// ============================================================

namespace client {
namespace {

// 等待 socket 上指定事件(锁外调用, 不阻塞其它方向的 SSL 操作)
static bool wait_io(int fd, short events) {
    struct pollfd p;
    p.fd = fd;
    p.events = events;
    p.revents = 0;
    for (;;) {
        int r = ::poll(&p, 1, -1);
        if (r > 0) return true;
        if (r < 0 && errno == EINTR) continue;
        return false;   // poll 出错
    }
}

// 阻塞读满 n 字节, 成功返回 true
bool recv_exact(int fd, char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        if (g_ssl != nullptr) {
            int r;
            {
                std::lock_guard<std::mutex> lk(g_ssl_mtx);
                r = SSL_read(g_ssl, buf + got, (int)(n - got));
            }
            if (r > 0) {
                got += static_cast<size_t>(r);
                continue;
            }
            switch (SSL_get_error(g_ssl, r)) {
                case SSL_ERROR_WANT_READ:
                    if (!wait_io(fd, POLLIN)) return false;
                    continue;
                case SSL_ERROR_WANT_WRITE:      // 罕见(重协商), 等可写后重试
                    if (!wait_io(fd, POLLOUT)) return false;
                    continue;
                case SSL_ERROR_ZERO_RETURN:
                    return false;               // 对端关闭(close_notify)
                case SSL_ERROR_SYSCALL:
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        if (!wait_io(fd, POLLIN)) return false;
                        continue;
                    }
                    return false;
                default:
                    return false;
            }
        }

        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r > 0) {
            got += static_cast<size_t>(r);
        } else if (r == 0) {
            return false;  // 对端关闭
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!wait_io(fd, POLLIN)) return false;
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// 阻塞发满 n 字节, 成功返回 true (MSG_NOSIGNAL 避免 SIGPIPE 终止进程)
bool send_exact(int fd, const char* data, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        if (g_ssl != nullptr) {
            // 单次 SSL_write 上限 64KB: 大于 socket 发送缓冲的整包写入会走 OpenSSL
            // 内部缓冲 + WANT_WRITE 反复重试路径, 吞吐骤降(大文件分片 1MB 尤其明显)。
            int r;
            {
                std::lock_guard<std::mutex> lk(g_ssl_mtx);
                int len = (int)std::min<size_t>(64 * 1024, n - sent);
                r = SSL_write(g_ssl, data + sent, len);
            }
            if (r > 0) {
                sent += static_cast<size_t>(r);
                continue;
            }
            switch (SSL_get_error(g_ssl, r)) {
                case SSL_ERROR_WANT_WRITE:
                    // 明文保留在 SSL 内部待刷: sent 未前移, 以相同参数重试
                    if (!wait_io(fd, POLLOUT)) return false;
                    continue;
                case SSL_ERROR_WANT_READ:       // 罕见(重协商), 等可读后重试
                    if (!wait_io(fd, POLLIN)) return false;
                    continue;
                default:
                    return false;
            }
        }

        ssize_t r = ::send(fd, data + sent, n - sent, MSG_NOSIGNAL);
        if (r > 0) {
            sent += static_cast<size_t>(r);
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_io(fd, POLLOUT)) return false;
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// 阻塞读取一个完整数据包(header + body), 失败/对端关闭返回空 vector
std::vector<char> read_packet(int fd) {
    protocol::packet_header hdr;
    if (!recv_exact(fd, reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
        return {};
    }
    if (hdr.magic != protocol::MAGIC_NUM) {
        fprintf(stderr, "[client] bad magic: 0x%04x\n", hdr.magic);
        return {};
    }
    if (hdr.body_len > protocol::MAX_BODY_LEN) {
        fprintf(stderr, "[client] body too large: %u\n", hdr.body_len);
        return {};
    }
    std::vector<char> packet(sizeof(hdr) + hdr.body_len);
    std::memcpy(packet.data(), &hdr, sizeof(hdr));
    if (hdr.body_len > 0) {
        if (!recv_exact(fd, packet.data() + sizeof(hdr), hdr.body_len)) {
            return {};
        }
    }
    return packet;
}

// 由请求分支推出应等待的响应分支
protocol::user::UserPacket::BodyCase expected_user_resp_case(const protocol::user::UserPacket& req) {
    using P = protocol::user::UserPacket;
    switch (req.body_case()) {
        case P::kRegisterReq:          return P::kRegisterResp;
        case P::kLoginReq:             return P::kLoginResp;
        case P::kLogoutReq:            return P::kLogoutResp;
        case P::kFriendRequestReq:     return P::kFriendRequestResp;
        case P::kFriendPendingListReq: return P::kFriendPendingListResp;
        case P::kFriendListReq:        return P::kFriendListResp;
        case P::kFriendDelReq:         return P::kFriendDelResp;
        case P::kFriendBlockReq:       return P::kFriendBlockResp;
        case P::kCancelReq:            return P::kCancelResp;
        default:                       return P::BODY_NOT_SET;
    }
}

protocol::chat::ChatPacket::BodyCase expected_chat_resp_case(const protocol::chat::ChatPacket& req) {
    using P = protocol::chat::ChatPacket;
    switch (req.body_case()) {
        case P::kSendReq:    return P::kSendResp;
        case P::kHistoryReq: return P::kHistoryResp;
        default:             return P::BODY_NOT_SET;
    }
}

protocol::group::GroupPacket::BodyCase expected_group_resp_case(const protocol::group::GroupPacket& req) {
    using P = protocol::group::GroupPacket;
    switch (req.body_case()) {
        case P::kCreateReq:       return P::kCreateResp;
        case P::kDissolveReq:     return P::kDissolveResp;
        case P::kPromoteAdminReq: return P::kPromoteAdminResp;
        case P::kJoinReq:         return P::kJoinResp;
        case P::kApproveJoinReq:  return P::kApproveJoinResp;
        case P::kRejectJoinReq:   return P::kRejectJoinResp;
        case P::kRemoveMemberReq: return P::kRemoveMemberResp;
        case P::kPendingListReq:  return P::kPendingListResp;
        case P::kMemberListReq:   return P::kMemberListResp;
        case P::kGroupListReq:    return P::kGroupListResp;
        case P::kQuitReq:         return P::kQuitResp;
        default:                  return P::BODY_NOT_SET;
    }
}

// ------------------------------------------------------------
//  后台收包线程: 常驻读 socket
//    推送类(ChatNotify/SystemNotify/UserStatusNotify) → 实时打印
//    响应类 → 入队, 由 user_request/chat_request 取回
// ------------------------------------------------------------
std::mutex              g_mtx;
std::condition_variable g_cv;
std::queue<std::vector<char>> g_queue;   // 待请求函数消费的响应包
bool g_closed = false;                    // 连接已关闭

void push_packet(std::vector<char> pkt) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_queue.push(std::move(pkt));
    g_cv.notify_one();
}

// 取下一个响应包; 连接已关闭返回空
std::vector<char> pop_packet() {
    std::unique_lock<std::mutex> lk(g_mtx);
    g_cv.wait(lk, [] { return g_closed || !g_queue.empty(); });
    if (g_queue.empty()) return {};
    auto pkt = std::move(g_queue.front());
    g_queue.pop();
    return pkt;
}

void reader_loop(int fd) {
    for (;;) {
        std::vector<char> pkt = read_packet(fd);
        if (pkt.empty()) {  // 对端关闭 / 出错
            std::lock_guard<std::mutex> lk(g_mtx);
            g_closed = true;
            g_cv.notify_all();
            return;
        }
        auto* hdr = reinterpret_cast<const protocol::packet_header*>(pkt.data());
        const char* body = pkt.data() + sizeof(*hdr);

        if (hdr->type == protocol::DOMAIN_CHAT) {
            protocol::chat::ChatPacket cp;
            if (cp.ParseFromArray(body, hdr->body_len) && cp.has_notify()) {
                const auto& m = cp.notify().msg();
                if (m.to_type() == protocol::chat::TARGET_TYPE_GROUP) {
                    fprintf(stdout, "\n[group<<] gid=%u from=%u: %s\n",
                            m.to_id(), m.from_id(), m.content().c_str());
                } else {
                    fprintf(stdout, "\n[chat<<] from=%u: %s\n", m.from_id(), m.content().c_str());
                }
                fflush(stdout);
                continue;   // 推送已打印, 不入队
            }
        } else if (hdr->type == protocol::DOMAIN_USER) {
            protocol::user::UserPacket up;
            if (up.ParseFromArray(body, hdr->body_len)) {
                if (up.has_heartbeat_resp()) {
                    continue;   // 心跳响应: 心跳线程只发不收, 直接忽略不入队
                }
                if (up.has_system_notify()) {
                    fprintf(stdout, "\n[system] %s\n", up.system_notify().content().c_str());
                    fflush(stdout);
                    continue;
                }
                if (up.has_user_status_notify()) {
                    const auto& s = up.user_status_notify();
                    fprintf(stdout, "\n[status] user=%u online=%d\n", s.user_id(), s.online());
                    fflush(stdout);
                    continue;
                }
            }
        } else if (hdr->type == protocol::DOMAIN_FILE) {
            protocol::file::FilePacket fp;
            if (fp.ParseFromArray(body, hdr->body_len) && fp.has_notify()) {
                const auto& n = fp.notify();
                fprintf(stdout, "\n[file<<] from=%u: %s (%llu bytes)\n",
                        n.from_id(), n.meta().name().c_str(),
                        static_cast<unsigned long long>(n.meta().size()));
                fflush(stdout);
                continue;   // 推送已打印, 不入队
            }
        }

        // 响应类 → 入队
        push_packet(std::move(pkt));
    }
}

}  // namespace

std::vector<char> build_packet(uint8_t type, const google::protobuf::Message& body) {
    std::string body_str;
    body.SerializeToString(&body_str);

    std::vector<char> packet;
    packet.resize(sizeof(protocol::packet_header) + body_str.size());

    auto* hdr = reinterpret_cast<protocol::packet_header*>(packet.data());
    hdr->magic    = protocol::MAGIC_NUM;
    hdr->ver      = 1;
    hdr->type     = type;
    hdr->body_len = static_cast<uint32_t>(body_str.size());

    if (!body_str.empty()) {
        std::memcpy(packet.data() + sizeof(protocol::packet_header),
                    body_str.data(), body_str.size());
    }
    return packet;
}

bool send_packet(int fd, const std::vector<char>& packet) {
    return send_exact(fd, packet.data(), packet.size());
}

void start_reader(int fd) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_closed = false;
        while (!g_queue.empty()) g_queue.pop();  // 清空历史残留
    }
    std::thread(reader_loop, fd).detach();
}

// 后台心跳线程: 周期性发送 HeartbeatReq, 连接关闭即退出。
// 服务端据此判定连接活性(收不到包的连接会视为登出被踢)。
void start_heartbeat(int fd) {
    static constexpr int kHeartbeatIntervalS = 5;
    std::thread([fd]() {
        protocol::user::UserPacket req;
        req.mutable_heartbeat_req();
        std::vector<char> pkt = build_packet(protocol::DOMAIN_USER, req);
        while (!g_closed) {
            std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatIntervalS));
            if (g_closed) break;
            if (!send_packet(fd, pkt)) {
                fprintf(stderr, "[heartbeat] send failed (connection closed?)\n");
                break;
            }
        }
    }).detach();
}  

bool user_request(int fd, protocol::user::UserPacket& req, protocol::user::UserPacket& resp) {
    std::vector<char> packet = build_packet(protocol::DOMAIN_USER, req);
    if (!send_exact(fd, packet.data(), packet.size())) {
        fprintf(stderr, "[client] send failed\n");
        return false;
    }
    protocol::user::UserPacket::BodyCase expect = expected_user_resp_case(req);
    for (;;) {
        std::vector<char> pkt = pop_packet();
        if (pkt.empty()) {
            fprintf(stderr, "[client] connection closed\n");
            return false;
        }
        auto* hdr = reinterpret_cast<const protocol::packet_header*>(pkt.data());
        if (hdr->type != protocol::DOMAIN_USER) {
            fprintf(stdout, "[client] <skip> non-user domain=%u\n", hdr->type);
            continue;
        }
        protocol::user::UserPacket r;
        if (!r.ParseFromArray(pkt.data() + sizeof(*hdr), hdr->body_len)) {
            fprintf(stdout, "[client] <skip> bad user body\n");
            continue;
        }
        if (r.body_case() == expect) {
            resp = std::move(r);
            return true;
        }
        fprintf(stdout, "[client] <skip> user case=%d\n", static_cast<int>(r.body_case()));
    }
}

bool chat_request(int fd, protocol::chat::ChatPacket& req, protocol::chat::ChatPacket& resp) {
    std::vector<char> packet = build_packet(protocol::DOMAIN_CHAT, req);
    if (!send_exact(fd, packet.data(), packet.size())) {
        fprintf(stderr, "[client] send failed\n");
        return false;
    }
    protocol::chat::ChatPacket::BodyCase expect = expected_chat_resp_case(req);
    for (;;) {
        std::vector<char> pkt = pop_packet();
        if (pkt.empty()) { 
            fprintf(stderr, "[client] connection closed\n");
            return false; 
        }
        auto* hdr = reinterpret_cast<const protocol::packet_header*>(pkt.data());
        if (hdr->type != protocol::DOMAIN_CHAT) {
            fprintf(stdout, "[client] <skip> non-chat domain=%u\n", hdr->type);
            continue;
        }
        protocol::chat::ChatPacket r;
        if (!r.ParseFromArray(pkt.data() + sizeof(*hdr), hdr->body_len)) {
            fprintf(stdout, "[client] <skip> bad chat body\n");
            continue;
        }
        if (r.body_case() == expect) {
            resp = std::move(r);
            return true;
        }
        fprintf(stdout, "[client] <skip> chat case=%d\n", static_cast<int>(r.body_case()));
    }
}

bool group_request(int fd, protocol::group::GroupPacket& req, protocol::group::GroupPacket& resp) {
    std::vector<char> packet = build_packet(protocol::DOMAIN_GROUP, req);
    if (!send_exact(fd, packet.data(), packet.size())) {
        fprintf(stderr, "[client] send failed\n");
        return false;
    }
    protocol::group::GroupPacket::BodyCase expect = expected_group_resp_case(req);
    for (;;) {
        std::vector<char> pkt = pop_packet();
        if (pkt.empty()) {
            fprintf(stderr, "[client] connection closed\n");
            return false;
        }
        auto* hdr = reinterpret_cast<const protocol::packet_header*>(pkt.data());
        if (hdr->type != protocol::DOMAIN_GROUP) {
            fprintf(stdout, "[client] <skip> non-group domain=%u\n", hdr->type);
            continue;
        }
        protocol::group::GroupPacket r;
        if (!r.ParseFromArray(pkt.data() + sizeof(*hdr), hdr->body_len)) {
            fprintf(stdout, "[client] <skip> bad group body\n");
            continue;
        }
        if (r.body_case() == expect) {
            resp = std::move(r);
            return true;
        }
        fprintf(stdout, "[client] <skip> group case=%d\n", static_cast<int>(r.body_case()));
    }
}

protocol::file::FilePacket::BodyCase expected_file_resp_case(const protocol::file::FilePacket& req) {
    using P = protocol::file::FilePacket;
    switch (req.body_case()) {
        case P::kListReq:      return P::kListResp;
        case P::kStatReq:      return P::kStatResp;
        case P::kSendReq:      return P::kSendResp;
        case P::kChunk:        return P::kChunkAck;
        case P::kSendFinish:   return P::kSendAck;
        case P::kGetReq:       return P::kGetResp;
        default:               return P::BODY_NOT_SET;
    }
}

bool file_request(int fd, protocol::file::FilePacket& req, protocol::file::FilePacket& resp) {
    std::vector<char> packet = build_packet(protocol::DOMAIN_FILE, req);
    if (!send_exact(fd, packet.data(), packet.size())) {
        fprintf(stderr, "[client] send failed\n");
        return false;
    }
    protocol::file::FilePacket::BodyCase expect = expected_file_resp_case(req);
    for (;;) {
        std::vector<char> pkt = pop_packet();
        if (pkt.empty()) {
            fprintf(stderr, "[client] connection closed\n");
            return false;
        }
        auto* hdr = reinterpret_cast<const protocol::packet_header*>(pkt.data());
        if (hdr->type != protocol::DOMAIN_FILE) {
            fprintf(stdout, "[client] <skip> non-file domain=%u\n", hdr->type);
            continue;
        }
        protocol::file::FilePacket r;
        if (!r.ParseFromArray(pkt.data() + sizeof(*hdr), hdr->body_len)) {
            fprintf(stdout, "[client] <skip> bad file body\n");
            continue;
        }
        if (r.body_case() == expect) {
            resp = std::move(r);
            return true;
        }
        fprintf(stdout, "[client] <skip> file case=%d\n", static_cast<int>(r.body_case()));
    }
}

}  // namespace client

// ------------------------------------------------------------
//  命令行
// ------------------------------------------------------------

namespace {

const char* err_name(int e) {
    switch (e) {
        case 0: return "SUCCESS";
        case 1: return "SYSTEM";
        case 2: return "INVALID_PARAM";
        case 3: return "INVALID_USER";
        case 4: return "USER_EXISTS";
        case 5: return "NOT_LOGGED_IN";
        case 6: return "FULL";
        case 7: return "NOT_FRIEND";
        case 8: return "ALREADY_FRIEND";
        case 9: return "BLOCKED";
        case 10: return "REQUEST_PENDING";
        case 11: return "GROUP_NOT_FOUND";
        case 12: return "NOT_GROUP_MEMBER";
        case 13: return "NOT_GROUP_ADMIN";
        case 14: return "NOT_GROUP_OWNER";
        case 15: return "ALREADY_IN_GROUP";
        case 16: return "GROUP_OWNER";
        default: return "?";
    }
}

void print_help() {
    fprintf(stdout,
            "commands:\n"
            "  register <username> <password> <nickname>\n"
            "  login    <username> <password>\n"
            "  logout\n"
            "  cancel   <username> <password>\n"
            "  friend req     <friend_id> [remark]\n"
            "  friend pending\n"
            "  friend list\n"
            "  friend del     <friend_id>\n"
            "  friend block   <friend_id> [on|off]\n"
            "  chat     <to_id> <text>\n"
            "  history  <target_id> [limit]\n"
            "  gcreate  <name>\n"
            "  gdissolve <group_id>\n"
            "  gjoin    <group_id> [remark]\n"
            "  gapprove <group_id> <user_id>\n"
            "  greject  <group_id> <user_id>\n"
            "  gpending <group_id>\n"
            "  gmembers <group_id>\n"
            "  gremove  <group_id> <user_id>\n"
            "  gpromote <group_id> <user_id>\n"
            "  gquit    <group_id>             leave a group (owner must dissolve instead)\n"
            "  glist\n"
            "  gchat    <group_id> <text>\n"
            "  ghistory <group_id> [limit]\n"
            "  fsend    <to_id> <filename>     upload client/files/cli_send/<filename> to <to_id>'s inbox (auto-resume)\n"
            "  fget     <filename>             download <filename> to client/files/cli_recv/ (auto-resume)\n"
            "  flist                          list files in my inbox\n"
            "  fstat    <filename>             query server-side size of <filename>\n"
            "  help\n"
            "  quit / exit\n");
}

// 向服务端发送登出请求并等待响应; 返回是否登出成功。
// 连接已断开/请求失败时返回 false(退出流程不阻塞, 直接继续关闭)。
bool send_logout(int fd, uint32_t uid) {
    protocol::user::UserPacket req;
    req.mutable_logout_req()->set_user_id(uid);
    protocol::user::UserPacket resp;
    if (!client::user_request(fd, req, resp)) {
        fprintf(stderr, "[logout] request failed (connection closed?)\n");
        return false;
    }
    const int err = resp.logout_resp().err();
    fprintf(stdout, "[logout] err=%s(%d)\n", err_name(err), err);
    return err == protocol::user::ERR_SUCCESS;
}

// 取 basename(与服务器 sanitize 规则对齐, 防本地路径穿越)
std::string client_basename(const std::string& raw) {
    std::string name = raw;
    size_t pos = name.find_last_of('/');
    if (pos != std::string::npos) name = name.substr(pos + 1);
    pos = name.find_last_of('\\');
    if (pos != std::string::npos) name = name.substr(pos + 1);
    return name;
}

// 客户端本地文件目录: 上传从这里找文件 —— 由可执行文件位置解析到项目根下,
// 不手动指定路径, 与启动时的工作目录无关(避免从非仓库根启动时 files 落到别处)。
// 目录(cli_send/cli_recv)为仓库内已存在的目录, 代码不创建。
// 解析后形如 <项目根>/client/files/cli_send。
static std::string client_file_dir() {
    static const std::string d = project_root() + "/client/files/cli_send";
    return d;
}
// 下载目录: 与上传源目录(cli_send)分开。若共用同一目录, fget 会把同名的上传源文件误当成
// 断点续传残片(直接跳过下载), 且下载会覆盖上传源 —— 单机双客户端测试必然踩中。
// (仿 FTP 的 STOR/RETR 上传/下载目录分离思路) 形如 <项目根>/client/files/cli_recv。
static std::string client_recv_dir() {
    static const std::string d = project_root() + "/client/files/cli_recv";
    return d;
}

// 上传 client/files/cli_send/<name> 到指定用户的收件箱。断点续传逻辑仿 ftp handle_stor:
//   FileSendReq 返回服务器已存字节数 → 本地 lseek 到该偏移 → 逐片
//   FileChunk(每片等 FileChunkAck) → FileSendFinish。
// 中途失败返回 false, 已上传部分留在服务器, 重跑即可续传。
bool upload_file(int fd, uint32_t to_id, const std::string& name_arg) {
    std::string name = client_basename(name_arg);   // 只按文件名在固定目录里找
    if (name.empty()) {
        fprintf(stderr, "[fsend] empty filename\n");
        return false;
    }
    std::string path = client_file_dir() + "/" + name;
    int lfd = ::open(path.c_str(), O_RDONLY);
    if (lfd < 0) {
        fprintf(stderr, "[fsend] cannot open %s (put the file under %s/)\n",
                path.c_str(), client_file_dir().c_str());
        return false;
    }
    off_t local_size = ::lseek(lfd, 0, SEEK_END);
    ::lseek(lfd, 0, SEEK_SET);

    protocol::file::FilePacket req;
    auto* sr = req.mutable_send_req();
    sr->set_to_id(to_id);
    sr->set_name(name);
    sr->set_size(static_cast<uint64_t>(local_size));
    protocol::file::FilePacket resp;
    if (!client::file_request(fd, req, resp)) {
        ::close(lfd);
        return false;
    }
    const auto& rr = resp.send_resp();
    if (rr.err() != protocol::user::ERR_SUCCESS) {
        fprintf(stdout, "[fsend] err=%s(%d)\n", err_name(rr.err()), static_cast<int>(rr.err()));
        ::close(lfd);
        return false;
    }
    uint64_t offset = rr.offset();
    if (offset > static_cast<uint64_t>(local_size)) offset = static_cast<uint64_t>(local_size);
    ::lseek(lfd, static_cast<off_t>(offset), SEEK_SET);
    fprintf(stdout, "[fsend] %s -> user %u, server offset=%llu (resume from here)\n",
            name.c_str(), to_id, static_cast<unsigned long long>(offset));

    std::vector<char> chunk_buf(protocol::FILE_CHUNK_SIZE);   // 堆上分片缓冲(单片 1MB)
    bool ok = true;
    while (offset < static_cast<uint64_t>(local_size)) {
        ssize_t n = ::read(lfd, chunk_buf.data(), chunk_buf.size());
        if (n <= 0) break;
        protocol::file::FilePacket creq;
        auto* c = creq.mutable_chunk();
        c->set_offset(offset);
        c->set_data(chunk_buf.data(), static_cast<size_t>(n));
        protocol::file::FilePacket cresp;
        if (!client::file_request(fd, creq, cresp)) { ok = false; break; }
        if (cresp.chunk_ack().err() != protocol::user::ERR_SUCCESS) {;
            fprintf(stderr, "[fsend] chunk ack err=%d\n",
                    static_cast<int>(cresp.chunk_ack().err()));
            ok = false;
            break;
        }
        offset += static_cast<uint64_t>(n);
    }
    ::close(lfd);
    if (!ok) {
        fprintf(stderr, "[fsend] interrupted at offset %llu (re-run fsend to resume)\n",
                static_cast<unsigned long long>(offset));
        return false;
    }

    protocol::file::FilePacket freq;
    freq.mutable_send_finish()->set_name(name);
    protocol::file::FilePacket fresp;
    if (!client::file_request(fd, freq, fresp)) return false;
    const auto& ack = fresp.send_ack();
    fprintf(stdout, "[fsend] done err=%s(%d) server_size=%llu\n",
            err_name(ack.err()), static_cast<int>(ack.err()),
            static_cast<unsigned long long>(ack.size()));
    return ack.err() == protocol::user::ERR_SUCCESS;
}

// 从自己收件箱下载文件到 client/files/cli_recv/。断点续传逻辑仿 ftp handle_retr:
//   本地已有部分取其大小作 offset → 逐片 FileGetReq → 追加写入 → eof 结束。
bool download_file(int fd, const std::string& raw_name) {
    std::string name = client_basename(raw_name);
    if (name.empty()) {
        fprintf(stderr, "[fget] empty filename\n");
        return false;
    }
    std::string path = client_recv_dir() + "/" + name;

    // 本地已有部分则取其大小作续传起点
    uint64_t offset = 0;
    int lfd = ::open(path.c_str(), O_RDONLY);
    if (lfd >= 0) {
        off_t sz = ::lseek(lfd, 0, SEEK_END);
        ::close(lfd);
        if (sz > 0) offset = static_cast<uint64_t>(sz);
    }
    lfd = -1;

    bool eof = false;
    uint64_t total = 0;
    while (!eof) {
        protocol::file::FilePacket req;
        auto* g = req.mutable_get_req();
        g->set_name(name);
        g->set_offset(offset);
        protocol::file::FilePacket resp;
        if (!client::file_request(fd, req, resp)) return false;
        const auto& gr = resp.get_resp();
        if (gr.err() != protocol::user::ERR_SUCCESS) {
            fprintf(stdout, "[fget] err=%s(%d)\n", err_name(gr.err()), static_cast<int>(gr.err()));
            return false;
        }
        total = gr.total();
        eof = gr.eof();
        // 先落盘本片数据(即使本片带 eof 标志, 也要写), 再退出循环
        const std::string& data = gr.data();
        if (!data.empty()) {
            if (lfd < 0) {
                lfd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (lfd < 0) {
                    fprintf(stderr, "[fget] cannot create %s: %s\n", path.c_str(), strerror(errno));
                    return false;
                }
            }
            size_t written = 0;
            while (written < data.size()) {
                ssize_t n = ::write(lfd, data.data() + written, data.size() - written);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                    ::close(lfd);
                    fprintf(stderr, "[fget] write failed\n");
                    return false;
                }
                written += static_cast<size_t>(n);
            }
            offset += data.size();
        }
    }
    if (lfd >= 0) {
        ::close(lfd);
        lfd = -1;
    }
    // 服务器文件比本地小时(服务器端被覆盖), 截断本地多余部分
    if (offset > total) {
        ::truncate(path.c_str(), static_cast<off_t>(total));
        offset = total;
    }
    fprintf(stdout, "[fget] %s downloaded: %llu bytes\n", path.c_str(),
            static_cast<unsigned long long>(offset));
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    // 忽略 SIGPIPE: TLS 路径的 SSL_write 可能向已断开的 socket 写(如连接被拒后
    // 仍做握手), 默认会 SIGPIPE 杀死进程并丢掉未刷新的 stdout。忽略后返回 EPIPE, 能干净报错。
    signal(SIGPIPE, SIG_IGN);

    // 仅提示文件目录位置(目录为仓库内已存在的 cli_send/cli_recv, 代码不创建)。
    fprintf(stdout, "[client] file dirs: send %s/  recv %s/ (put files to send into the send dir)\n",
            client_file_dir().c_str(), client_recv_dir().c_str());

    const char* ip = "127.0.0.1";
    int port = 2100;
    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) ip = argv[2];

    network::Socket sock;
    if (sock.connect(ip, port) != 0) {
        fprintf(stderr, "[client] connect %s:%d failed\n", ip, port);
        return 1;
    }
    int fd = sock.fd();

    // ---- TLS 握手(握手期间保持阻塞; 完成后转非阻塞 + 互斥锁并发读写) ----
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
        fprintf(stderr, "[client] SSL_CTX_new failed\n");
        return 1;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);
    // 开发环境跳过自签名证书校验。生产环境应改为 SSL_VERIFY_PEER +
    // SSL_CTX_load_verify_locations(ca) 并检查 SSL_get_verify_result。
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    SSL* ssl = SSL_new(ctx);
    if (ssl == nullptr) {
        fprintf(stderr, "[client] SSL_new failed\n");
        SSL_CTX_free(ctx);
        return 1;
    }
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) {
        fprintf(stderr, "[client] TLS handshake failed: ");
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return 1;
    }
    g_ssl = ssl;
    sock.set_nonblock();   // 此后由 poll 驱动等待, 避免持锁阻塞其它方向的 SSL 操作
    fprintf(stdout, "[client] connected to %s:%d (fd=%d) tls=%s\n",
            ip, port, fd, SSL_get_version(ssl));
    print_help();

    client::start_reader(fd);   // 后台线程常驻收包, 推送实时打印
    client::start_heartbeat(fd);  // 后台线程周期发心跳, 保持连接活性

    uint32_t g_uid = 0;        // 当前登录用户 id
    char g_username[128] = {0};  // 当前登录用户名(用于判断注销的是否为当前账号)

    char line[1024];
    while (1) {
        // 服务端主动断开连接(如被顶号/被踢): 及时提示并退出, 不必等用户输入才发现。
        // (被顶号时, 服务端会先推 SystemNotify, 再由 reader 线程置 g_closed)
        if (client::g_closed) {
            fprintf(stdout, "\n[client] connection closed by server\n");
            break;
        }
        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = ::poll(&pfd, 1, 200);   // 200ms 轮询, 以便及时感知 g_closed
        if (pr == 0) continue;           // 超时: 回头检查 g_closed
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        // POLLIN=有输入; POLLHUP=对端已关(EOF)但可能还有缓冲数据未读 —— 都尝试读取,
        // fgets 读到 EOF 返回 nullptr。忽略其它标志继续轮询。
        if (!(pfd.revents & (POLLIN | POLLHUP))) continue;

        fprintf(stdout, "\nclient> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == nullptr) break;  // EOF
        line[strcspn(line, "\n")] = '\0';

        char cmd[64] = {0};
        if (sscanf(line, "%63s", cmd) != 1) continue;

        if (strcmp(cmd, "help") == 0) {
            print_help();

        } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            break;

        } else if (strcmp(cmd, "register") == 0) {
            char username[128] = {0}, password[128] = {0}, nickname[128] = {0};
            if (sscanf(line, "%*s %127s %127s %127s", username, password, nickname) != 3) {
                fprintf(stderr, "usage: register <username> <password> <nickname>\n");
                continue;
            }
            protocol::user::UserPacket req;   
            auto* r = req.mutable_register_req();
            r->set_username(username);
            r->set_password(password);
            r->set_nickname(nickname);
            protocol::user::UserPacket resp;
            if (client::user_request(fd, req, resp)) {
                const auto& rr = resp.register_resp();
                fprintf(stdout, "[register] err=%s(%d) user_id=%u\n",
                        err_name(rr.err()), static_cast<int>(rr.err()), rr.user_id());
            }

        } else if (strcmp(cmd, "login") == 0) {
            char username[128] = {0}, password[128] = {0};
            if (sscanf(line, "%*s %127s %127s", username, password) != 2) {
                fprintf(stderr, "usage: login <username> <password>\n");
                continue;
            } 
            protocol::user::UserPacket req;
            auto* r = req.mutable_login_req();
            r->set_username(username);
            r->set_password(password);
            protocol::user::UserPacket resp;
            if (client::user_request(fd, req, resp)) {
                const auto& rr = resp.login_resp();
                fprintf(stdout, "[login] err=%s(%d)", err_name(rr.err()), static_cast<int>(rr.err()));
                if (rr.err() == protocol::user::ERR_SUCCESS) {
                    g_uid = rr.user_id(); 
                    snprintf(g_username, sizeof(g_username), "%s", username);
                    fprintf(stdout, " user_id=%u nickname=%s",  
                            rr.user_id(), rr.user().nickname().c_str());
                }
                fprintf(stdout, "\n");
            }

        } else if (strcmp(cmd, "logout") == 0) {
            if (g_uid == 0) {
                fprintf(stdout, "[error] login first");
            } else if (send_logout(fd, g_uid)) {
                g_uid = 0;
                g_username[0] = '\0';
            }

        } else if (strcmp(cmd, "cancel") == 0) {
            char username[128] = {0}, password[128] = {0};
            if (sscanf(line, "%*s %127s %127s", username, password) != 2) {
                fprintf(stderr, "usage: cancel <username> <password>\n");
                continue;
            }
            protocol::user::UserPacket req;
            auto* r = req.mutable_cancel_req();
            r->set_username(username);
            r->set_password(password);
            protocol::user::UserPacket resp; 
            if (client::user_request(fd, req, resp)) {
                const auto& rr = resp.cancel_resp();
                fprintf(stdout, "[cancel] err=%s(%d)\n",
                        err_name(rr.err()), static_cast<int>(rr.err()));
                // 注销的正是当前登录账号: 本地清空登录态(服务端已解绑)
                if (rr.err() == protocol::user::ERR_SUCCESS &&
                    g_uid != 0 && strcmp(username, g_username) == 0) {
                    g_uid = 0;
                    g_username[0] = '\0';
                }
            }

        } else if (strcmp(cmd, "friend") == 0) {
            char sub[64] = {0};
            if (sscanf(line, "%*s %63s", sub) != 1) {
                fprintf(stderr, "usage: friend req|pending|list|del|block ...\n");
                continue;
            }
            if (strcmp(sub, "req") == 0) {
                uint32_t fid = 0;
                char remark[128] = {0};
                if (sscanf(line, "%*s %*s %u %127[^\n]", &fid, remark) < 1) {
                    fprintf(stderr, "usage: friend req <friend_id> [remark]\n");
                    continue;
                }
                protocol::user::UserPacket req;
                auto* r = req.mutable_friend_request_req();
                r->set_friend_id(fid);
                r->set_remark(remark);
                protocol::user::UserPacket resp;
                if (client::user_request(fd, req, resp)) {
                    fprintf(stdout, "[friend.req] err=%s(%d) friend_id=%u\n",
                            err_name(resp.friend_request_resp().err()),
                            static_cast<int>(resp.friend_request_resp().err()),
                            resp.friend_request_resp().friend_id());
                }
            } else if (strcmp(sub, "pending") == 0) {
                protocol::user::UserPacket req;
                req.mutable_friend_pending_list_req();
                protocol::user::UserPacket resp;
                if (client::user_request(fd, req, resp)) {
                    const auto& rr = resp.friend_pending_list_resp();
                    fprintf(stdout, "[friend.pending] err=%s(%d) items=%d\n",
                            err_name(rr.err()), static_cast<int>(rr.err()), rr.items_size());
                    for (const auto& it : rr.items()) {
                        fprintf(stdout, "  %u  %s  remark='%s' ts=%llu\n",
                                it.friend_id(), it.nickname().c_str(), it.remark().c_str(),
                                static_cast<unsigned long long>(it.ts()));
                    }
                }
            } else if (strcmp(sub, "list") == 0) {
                protocol::user::UserPacket req;
                req.mutable_friend_list_req();
                protocol::user::UserPacket resp;
                if (client::user_request(fd, req, resp)) {
                    const auto& rr = resp.friend_list_resp();
                    fprintf(stdout, "[friend.list] err=%s(%d) friends=%d\n",
                            err_name(rr.err()), static_cast<int>(rr.err()), rr.friends_size());
                    for (const auto& it : rr.friends()) {
                        fprintf(stdout, "  %-8u %-20s %s\n", it.user_id(), it.nickname().c_str(),
                                it.online() ? "online" : "offline");
                    }
                }
            } else if (strcmp(sub, "del") == 0) {
                uint32_t fid = 0;
                if (sscanf(line, "%*s %*s %u", &fid) != 1) {
                    fprintf(stderr, "usage: friend del <friend_id>\n");
                    continue;
                }
                protocol::user::UserPacket req;
                req.mutable_friend_del_req()->set_friend_id(fid);
                protocol::user::UserPacket resp;
                if (client::user_request(fd, req, resp)) {
                    fprintf(stdout, "[friend.del] err=%s(%d)\n",
                            err_name(resp.friend_del_resp().err()),
                            static_cast<int>(resp.friend_del_resp().err()));
                }
            } else if (strcmp(sub, "block") == 0) {
                uint32_t fid = 0;
                char mode[8] = {0};
                if (sscanf(line, "%*s %*s %u %7s", &fid, mode) < 1) {
                    fprintf(stderr, "usage: friend block <friend_id> [on|off]\n");
                    continue;
                }
                bool block = (mode[0] == '\0') || (strcmp(mode, "on") == 0);
                protocol::user::UserPacket req;
                auto* r = req.mutable_friend_block_req();
                r->set_friend_id(fid);
                r->set_block(block);
                protocol::user::UserPacket resp;
                if (client::user_request(fd, req, resp)) {
                    fprintf(stdout, "[friend.block] err=%s(%d)\n",
                            err_name(resp.friend_block_resp().err()),
                            static_cast<int>(resp.friend_block_resp().err()));
                }
            } else {
                fprintf(stderr, "usage: friend req|pending|list|del|block ...\n");
            }

        } else if (strcmp(cmd, "chat") == 0) {
            uint32_t to_id = 0;
            char content[4096] = {0};
            if (sscanf(line, "%*s %u %4095[^\n]", &to_id, content) < 1) {
                fprintf(stderr, "usage: chat <to_id> <text>\n");
                continue;
            }
            protocol::chat::ChatPacket req;
            auto* r = req.mutable_send_req();
            r->set_to_id(to_id);
            r->set_content(content);
            protocol::chat::ChatPacket resp;
            if (client::chat_request(fd, req, resp)) {
                const auto& rr = resp.send_resp();
                fprintf(stdout, "[chat] err=%s(%d) msg_id=%llu server_ts=%llu\n",
                        err_name(rr.err()), static_cast<int>(rr.err()),
                        static_cast<unsigned long long>(rr.msg_id()),
                        static_cast<unsigned long long>(rr.server_ts()));
            }

        } else if (strcmp(cmd, "history") == 0) {
            uint32_t target_id = 0;
            uint32_t limit = 50;
            int n = sscanf(line, "%*s %u %u", &target_id, &limit);
            if (n < 1) {
                fprintf(stderr, "usage: history <target_id> [limit]\n");
                continue;
            }
            protocol::chat::ChatPacket req;
            auto* r = req.mutable_history_req();
            r->set_target_id(target_id); 
            r->set_after_msg_id(0);
            r->set_limit(limit);
            protocol::chat::ChatPacket resp;
            if (client::chat_request(fd, req, resp)) {
                const auto& rr = resp.history_resp();
                fprintf(stdout, "[history] err=%s(%d) messages=%d\n",
                        err_name(rr.err()), static_cast<int>(rr.err()), rr.messages_size());
                for (const auto& m : rr.messages()) {
                    fprintf(stdout, "  #%llu %s(%u) -> %u: %s\n",
                            static_cast<unsigned long long>(m.msg_id()),
                            m.from_id() == g_uid ? "me" : "peer",
                            m.from_id(), m.to_id(), m.content().c_str());
                }
            }

        } else if (strcmp(cmd, "gcreate") == 0) {
            char name[64] = {0};
            if (sscanf(line, "%*s %63s", name) != 1) {
                fprintf(stderr, "usage: gcreate <name>\n");
                continue;
            }
            protocol::group::GroupPacket req;
            req.mutable_create_req()->set_name(name);
            protocol::group::GroupPacket resp;
            if (client::group_request(fd, req, resp)) {
                const auto& rr = resp.create_resp();
                fprintf(stdout, "[gcreate] err=%s(%d) group_id=%u\n",
                        err_name(rr.err()), static_cast<int>(rr.err()), rr.group_id());
            }

        } else if (strcmp(cmd, "gdissolve") == 0) {
            uint32_t gid = 0;
            if (sscanf(line, "%*s %u", &gid) != 1) {
                fprintf(stderr, "usage: gdissolve <group_id>\n");
                continue;
            }
            protocol::group::GroupPacket req;
            req.mutable_dissolve_req()->set_group_id(gid);
            protocol::group::GroupPacket resp;
            if (client::group_request(fd, req, resp)) {
                const auto& rr = resp.dissolve_resp();
                fprintf(stdout, "[gdissolve] err=%s(%d) group_id=%u\n",
                        err_name(rr.err()), static_cast<int>(rr.err()), rr.group_id());
            }

        } else if (strcmp(cmd, "gjoin") == 0) {
            uint32_t gid = 0;
            char remark[128] = {0};
            if (sscanf(line, "%*s %u %127[^\n]", &gid, remark) < 1) {
                fprintf(stderr, "usage: gjoin <group_id> [remark]\n");
                continue;
            }
            protocol::group::GroupPacket req;
            auto* r = req.mutable_join_req();
            r->set_group_id(gid);
            r->set_remark(remark);
            protocol::group::GroupPacket resp;
            if (client::group_request(fd, req, resp)) {
                const auto& rr = resp.join_resp();
                fprintf(stdout, "[gjoin] err=%s(%d) group_id=%u\n",
                        err_name(rr.err()), static_cast<int>(rr.err()), rr.group_id());
            }

        } else if (strcmp(cmd, "gapprove") == 0) {
            uint32_t gid = 0, uid = 0;
            if (sscanf(line, "%*s %u %u", &gid, &uid) != 2) {
                fprintf(stderr, "usage: gapprove <group_id> <user_id>\n");
                continue;
            }
            protocol::group::GroupPacket req;
            auto* r = req.mutable_approve_join_req();
            r->set_group_id(gid);
            r->set_user_id(uid);
            protocol::group::GroupPacket resp;
            if (client::group_request(fd, req, resp)) {
                const auto& rr = resp.approve_join_resp();
                fprintf(stdout, "[gapprove] err=%s(%d) group_id=%u user_id=%u\n",
                        err_name(rr.err()), static_cast<int>(rr.err()),
                        rr.group_id(), rr.user_id());
            }

        } else if (strcmp(cmd, "greject") == 0) {
            uint32_t gid = 0, uid = 0;
            if (sscanf(line, "%*s %u %u", &gid, &uid) != 2) {
                fprintf(stderr, "usage: greject <group_id> <user_id>\n");
                continue;
            }
            protocol::group::GroupPacket req;
            auto* r = req.mutable_reject_join_req();
            r->set_group_id(gid);
            r->set_user_id(uid);
            protocol::group::GroupPacket resp;
            if (client::group_request(fd, req, resp)) {
                const auto& rr = resp.reject_join_resp();
                fprintf(stdout, "[greject] err=%s(%d) group_id=%u user_id=%u\n",
                        err_name(rr.err()), static_cast<int>(rr.err()),
                        rr.group_id(), rr.user_id());
            }

        } else if (strcmp(cmd, "gpending") == 0) {
            uint32_t gid = 0;
            if (sscanf(line, "%*s %u", &gid) != 1) {
                fprintf(stderr, "usage: gpending <group_id>\n");
                continue;
            }
            protocol::group::GroupPacket req;
            req.mutable_pending_list_req()->set_group_id(gid);
            protocol::group::GroupPacket resp;
            if (client::group_request(fd, req, resp)) {
                const auto& rr = resp.pending_list_resp();
                fprintf(stdout, "[gpending] err=%s(%d) items=%d\n",
                        err_name(rr.err()), static_cast<int>(rr.err()), rr.items_size());
                for (const auto& it : rr.items()) {
                    fprintf(stdout, "  %u  %s  remark='%s' ts=%llu\n",
                            it.user_id(), it.nickname().c_str(), it.remark().c_str(),
                            static_cast<unsigned long long>(it.ts()));
                }
            }

        } else if (strcmp(cmd, "gmembers") == 0) {
            uint32_t gid = 0;
            if (sscanf(line, "%*s %u", &gid) != 1) {
                fprintf(stderr, "usage: gmembers <group_id>\n");
                continue;
            }
            protocol::group::GroupPacket req;
            req.mutable_member_list_req()->set_group_id(gid);
            protocol::group::GroupPacket resp;
            if (client::group_request(fd, req, resp)) {
                const auto& rr = resp.member_list_resp();
                fprintf(stdout, "[gmembers] err=%s(%d) members=%d\n",
                        err_name(rr.err()), static_cast<int>(rr.err()), rr.members_size());
                for (const auto& it : rr.members()) {
                    fprintf(stdout, "  %u  %s  role=%d\n",
                            it.user_id(), it.nickname().c_str(), static_cast<int>(it.role()));
                }
            }

        } else if (strcmp(cmd, "gremove") == 0) {
            uint32_t gid = 0, uid = 0;
            if (sscanf(line, "%*s %u %u", &gid, &uid) != 2) {
                fprintf(stderr, "usage: gremove <group_id> <user_id>\n");
                continue;
            }
            protocol::group::GroupPacket req;
            auto* r = req.mutable_remove_member_req();
            r->set_group_id(gid);
            r->set_user_id(uid);
            protocol::group::GroupPacket resp;
            if (client::group_request(fd, req, resp)) {
                const auto& rr = resp.remove_member_resp();
                fprintf(stdout, "[gremove] err=%s(%d) group_id=%u user_id=%u\n",
                        err_name(rr.err()), static_cast<int>(rr.err()),
                        rr.group_id(), rr.user_id());
            }

        } else if (strcmp(cmd, "gpromote") == 0) {
            uint32_t gid = 0, uid = 0;
            if (sscanf(line, "%*s %u %u", &gid, &uid) != 2) {
                fprintf(stderr, "usage: gpromote <group_id> <user_id>\n");
                continue;
            }
            protocol::group::GroupPacket req;
            auto* r = req.mutable_promote_admin_req();
            r->set_group_id(gid);
            r->set_user_id(uid);
            protocol::group::GroupPacket resp;
            if (client::group_request(fd, req, resp)) {
                const auto& rr = resp.promote_admin_resp();
                fprintf(stdout, "[gpromote] err=%s(%d) group_id=%u user_id=%u\n",
                        err_name(rr.err()), static_cast<int>(rr.err()),
                        rr.group_id(), rr.user_id());
            }

        } else if (strcmp(cmd, "gquit") == 0) {
            uint32_t gid = 0;
            if (sscanf(line, "%*s %u", &gid) != 1) {
                fprintf(stderr, "usage: gquit <group_id>\n");
                continue;
            }
            protocol::group::GroupPacket req;
            req.mutable_quit_req()->set_group_id(gid);
            protocol::group::GroupPacket resp;
            if (client::group_request(fd, req, resp)) {
                const auto& rr = resp.quit_resp();
                fprintf(stdout, "[gquit] err=%s(%d) group_id=%u\n",
                        err_name(rr.err()), static_cast<int>(rr.err()), rr.group_id());
            }

        } else if (strcmp(cmd, "glist") == 0) {
            protocol::group::GroupPacket req;
            req.mutable_group_list_req();
            protocol::group::GroupPacket resp;
            if (client::group_request(fd, req, resp)) {
                const auto& rr = resp.group_list_resp();
                fprintf(stdout, "[glist] err=%s(%d) groups=%d\n",
                        err_name(rr.err()), static_cast<int>(rr.err()), rr.groups_size());
                for (const auto& it : rr.groups()) {
                    fprintf(stdout, "  %u  %s  role=%d\n",
                            it.group_id(), it.name().c_str(), static_cast<int>(it.role()));
                }
            }

        } else if (strcmp(cmd, "gchat") == 0) {
            uint32_t gid = 0;
            char content[4096] = {0};
            if (sscanf(line, "%*s %u %4095[^\n]", &gid, content) < 1) {
                fprintf(stderr, "usage: gchat <group_id> <text>\n");
                continue;
            }
            protocol::chat::ChatPacket req;
            auto* r = req.mutable_send_req();
            r->set_to_id(gid);
            r->set_to_type(protocol::chat::TARGET_TYPE_GROUP);
            r->set_content(content);
            protocol::chat::ChatPacket resp;
            if (client::chat_request(fd, req, resp)) {
                const auto& rr = resp.send_resp();
                fprintf(stdout, "[gchat] err=%s(%d) msg_id=%llu server_ts=%llu\n",
                        err_name(rr.err()), static_cast<int>(rr.err()),
                        static_cast<unsigned long long>(rr.msg_id()),
                        static_cast<unsigned long long>(rr.server_ts()));
            }

        } else if (strcmp(cmd, "ghistory") == 0) {
            uint32_t gid = 0;
            uint32_t limit = 50;
            int n = sscanf(line, "%*s %u %u", &gid, &limit);
            if (n < 1) {
                fprintf(stderr, "usage: ghistory <group_id> [limit]\n");
                continue;
            }
            protocol::chat::ChatPacket req;
            auto* r = req.mutable_history_req();
            r->set_target_id(gid);
            r->set_to_type(protocol::chat::TARGET_TYPE_GROUP);
            r->set_after_msg_id(0);
            r->set_limit(limit);
            protocol::chat::ChatPacket resp;
            if (client::chat_request(fd, req, resp)) {
                const auto& rr = resp.history_resp();
                fprintf(stdout, "[ghistory] err=%s(%d) messages=%d\n",
                        err_name(rr.err()), static_cast<int>(rr.err()), rr.messages_size());
                for (const auto& m : rr.messages()) {
                    fprintf(stdout, "  #%llu %s(%u) -> gid=%u: %s\n",
                            static_cast<unsigned long long>(m.msg_id()),
                            m.from_id() == g_uid ? "me" : "member",
                            m.from_id(), m.to_id(), m.content().c_str());
                }
            }

        } else if (strcmp(cmd, "fsend") == 0) {
            uint32_t to_id = 0;
            char fname[128] = {0};
            if (sscanf(line, "%*s %u %127s", &to_id, fname) != 2) {
                fprintf(stderr, "usage: fsend <to_id> <filename>   (looks in %s/)\n",
                        client_file_dir().c_str());
                continue;
            }
            if (g_uid == 0) {
                fprintf(stderr, "[error] login first\n");
                continue;
            }
            upload_file(fd, to_id, fname);

        } else if (strcmp(cmd, "fget") == 0) {
            char fname[128] = {0};
            if (sscanf(line, "%*s %127s", fname) != 1) {
                fprintf(stderr, "usage: fget <filename>\n");
                continue;
            }
            if (g_uid == 0) {
                fprintf(stderr, "[error] login first\n");
                continue;
            }
            download_file(fd, fname);

        } else if (strcmp(cmd, "flist") == 0) {
            if (g_uid == 0) {
                fprintf(stderr, "[error] login first\n");
                continue;
            }
            protocol::file::FilePacket req;
            req.mutable_list_req()->set_owner_id(g_uid);
            protocol::file::FilePacket resp;
            if (client::file_request(fd, req, resp)) {
                const auto& rr = resp.list_resp();
                fprintf(stdout, "[flist] err=%s(%d) files=%d\n",
                        err_name(rr.err()), static_cast<int>(rr.err()), rr.files_size());
                for (const auto& f : rr.files()) {
                    fprintf(stdout, "  %-40s %10llu bytes\n", f.name().c_str(),
                            static_cast<unsigned long long>(f.size()));
                }
            }

        } else if (strcmp(cmd, "fstat") == 0) {
            char fname[128] = {0};
            if (sscanf(line, "%*s %127s", fname) != 1) {
                fprintf(stderr, "usage: fstat <filename>\n");
                continue;
            }
            if (g_uid == 0) {
                fprintf(stderr, "[error] login first\n");
                continue;
            }
            protocol::file::FilePacket req;
            req.mutable_stat_req()->set_name(fname);
            protocol::file::FilePacket resp;
            if (client::file_request(fd, req, resp)) {
                const auto& rr = resp.stat_resp();
                fprintf(stdout, "[fstat] err=%s(%d) size=%llu\n",
                        err_name(rr.err()), static_cast<int>(rr.err()),
                        static_cast<unsigned long long>(rr.size()));
            }

        } else {
            fprintf(stderr, "unknown command: %s (try 'help')\n", cmd);
        }
    }

    // 退出前若已登录, 先向服务端登出(online 置 0, 并向好友推送下线通知)。
    // 覆盖 quit / exit / EOF 三种退出路径。
    if (g_uid != 0) {
        send_logout(fd, g_uid);
    }

    if (g_ssl != nullptr) {
        SSL_free(g_ssl);
        g_ssl = nullptr;
    }
    if (ctx != nullptr) {
        SSL_CTX_free(ctx);
        ctx = nullptr;
    }
    ::close(fd);
    fprintf(stdout, "\n[client] closed.\n");
    return 0;
}
