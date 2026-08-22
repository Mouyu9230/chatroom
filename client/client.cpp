#include "client.hpp"
#include "ui.hpp"

#include "../network/socket/socket.hpp"
#include "../network/project_path.hpp"
#include "../protocol/group/group.pb.h"

#include <readline/history.h>
#include <readline/readline.h>

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
#include <deque>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// ------------------------------------------------------------
//  TLS 全局状态(客户端单连接, 全局即可)
//    SSL 对象不支持多线程并发调用(即使方向不同), 因此所有 SSL 读写都必须持锁,
//    且等待期间不能持锁 —— 握手完成后连接转非阻塞, 由 poll 等待事件。
// ------------------------------------------------------------
SSL* g_ssl = nullptr;
std::mutex g_ssl_mtx;

// ============================================================
//  客户端实现  用法: client [ip] [port]  (默认 127.0.0.1:2100)
//  后台收包线程常驻读 socket 实时打印推送; 请求响应入队, 主线程取回。
//  UI 层: 彩色输出 / readline 历史补全 / 推送防打断重绘 / 会话目标模式。
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
        ui::push_printf(ui::Color::Red, "[client] bad magic: 0x%04x", hdr.magic);
        return {};
    }
    if (hdr.body_len > protocol::MAX_BODY_LEN) {
        ui::push_printf(ui::Color::Red, "[client] body too large: %u", hdr.body_len);
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

// reader 线程: 格式化并打印一条收到的聊天消息(昵称 + 会话目标高亮 ">> ")
static void print_incoming_chat(const protocol::chat::ChatMessage& m) {
    const bool group = m.to_type() == protocol::chat::TARGET_TYPE_GROUP;
    ui::Session* s = ui::g_session;
    std::string from = s ? s->nick.display(m.from_id()) : std::to_string(m.from_id());

    bool hl = false;
    if (s && s->has_target()) {
        hl = group ? (s->target_group && m.to_id() == s->target_id)
                   : (!s->target_group && m.from_id() == s->target_id);
    }
    if (group) {
        std::string gname = s ? s->groups.display(m.to_id()) : std::to_string(m.to_id());
        ui::push_printf(ui::Color::Yellow, "%s[群 %s(%u)] %s: %s",
                        hl ? ">> " : "", gname.c_str(), m.to_id(),
                        from.c_str(), m.content().c_str());
    } else {
        ui::push_printf(ui::Color::Cyan, "%s[%s] %s",
                        hl ? ">> " : "", from.c_str(), m.content().c_str());
    }
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
                print_incoming_chat(cp.notify().msg());
                continue;   // 推送已打印, 不入队
            }
        } else if (hdr->type == protocol::DOMAIN_USER) {
            protocol::user::UserPacket up;
            if (up.ParseFromArray(body, hdr->body_len)) {
                if (up.has_heartbeat_resp()) {
                    continue;   // 心跳响应: 心跳线程只发不收, 直接忽略不入队
                }
                if (up.has_system_notify()) {
                    ui::push_printf(ui::Color::Yellow, "[系统] %s",
                                    up.system_notify().content().c_str());
                    continue;
                }
                if (up.has_user_status_notify()) {
                    const auto& s = up.user_status_notify();
                    std::string who = ui::g_session
                        ? ui::g_session->nick.display(s.user_id())
                        : std::to_string(s.user_id());
                    ui::push_printf(ui::Color::Dim, "[状态] %s %s", who.c_str(),
                                    s.online() ? "上线" : "下线");
                    continue;
                }
            }
        } else if (hdr->type == protocol::DOMAIN_FILE) {
            protocol::file::FilePacket fp;
            if (fp.ParseFromArray(body, hdr->body_len) && fp.has_notify()) {
                const auto& n = fp.notify();
                std::string who = ui::g_session
                    ? ui::g_session->nick.display(n.from_id())
                    : std::to_string(n.from_id());
                ui::push_printf(ui::Color::Blue, "[来文件] %s 发来: %s (%llu 字节)",
                                who.c_str(), n.meta().name().c_str(),
                                static_cast<unsigned long long>(n.meta().size()));
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
                ui::push_printf(ui::Color::Red, "[心跳] 发送失败(连接已断开?)");
                break;
            }
        }
    }).detach();
}

bool user_request(int fd, protocol::user::UserPacket& req, protocol::user::UserPacket& resp) {
    std::vector<char> packet = build_packet(protocol::DOMAIN_USER, req);
    if (!send_exact(fd, packet.data(), packet.size())) {
        ui::printf(ui::Color::Red, "[客户端] 发送失败");
        return false;
    }
    protocol::user::UserPacket::BodyCase expect = expected_user_resp_case(req);
    for (;;) {
        std::vector<char> pkt = pop_packet();
        if (pkt.empty()) {
            ui::printf(ui::Color::Red, "[客户端] 连接已断开");
            return false;
        }
        auto* hdr = reinterpret_cast<const protocol::packet_header*>(pkt.data());
        if (hdr->type != protocol::DOMAIN_USER) {
            ui::printf(ui::Color::Dim, "[client] <skip> non-user domain=%u", hdr->type);
            continue;
        }
        protocol::user::UserPacket r;
        if (!r.ParseFromArray(pkt.data() + sizeof(*hdr), hdr->body_len)) {
            ui::printf(ui::Color::Dim, "[client] <skip> bad user body");
            continue;
        }
        if (r.body_case() == expect) {
            resp = std::move(r);
            return true;
        }
        ui::printf(ui::Color::Dim, "[client] <skip> user case=%d", static_cast<int>(r.body_case()));
    }
}

bool chat_request(int fd, protocol::chat::ChatPacket& req, protocol::chat::ChatPacket& resp) {
    std::vector<char> packet = build_packet(protocol::DOMAIN_CHAT, req);
    if (!send_exact(fd, packet.data(), packet.size())) {
        ui::printf(ui::Color::Red, "[客户端] 发送失败");
        return false;
    }
    protocol::chat::ChatPacket::BodyCase expect = expected_chat_resp_case(req);
    for (;;) {
        std::vector<char> pkt = pop_packet();
        if (pkt.empty()) {
            ui::printf(ui::Color::Red, "[客户端] 连接已断开");
            return false;
        }
        auto* hdr = reinterpret_cast<const protocol::packet_header*>(pkt.data());
        if (hdr->type != protocol::DOMAIN_CHAT) {
            ui::printf(ui::Color::Dim, "[client] <skip> non-chat domain=%u", hdr->type);
            continue;
        }
        protocol::chat::ChatPacket r;
        if (!r.ParseFromArray(pkt.data() + sizeof(*hdr), hdr->body_len)) {
            ui::printf(ui::Color::Dim, "[client] <skip> bad chat body");
            continue;
        }
        if (r.body_case() == expect) {
            resp = std::move(r);
            return true;
        }
        ui::printf(ui::Color::Dim, "[client] <skip> chat case=%d", static_cast<int>(r.body_case()));
    }
}

bool group_request(int fd, protocol::group::GroupPacket& req, protocol::group::GroupPacket& resp) {
    std::vector<char> packet = build_packet(protocol::DOMAIN_GROUP, req);
    if (!send_exact(fd, packet.data(), packet.size())) {
        ui::printf(ui::Color::Red, "[客户端] 发送失败");
        return false;
    }
    protocol::group::GroupPacket::BodyCase expect = expected_group_resp_case(req);
    for (;;) {
        std::vector<char> pkt = pop_packet();
        if (pkt.empty()) {
            ui::printf(ui::Color::Red, "[客户端] 连接已断开");
            return false;
        }
        auto* hdr = reinterpret_cast<const protocol::packet_header*>(pkt.data());
        if (hdr->type != protocol::DOMAIN_GROUP) {
            ui::printf(ui::Color::Dim, "[client] <skip> non-group domain=%u", hdr->type);
            continue;
        }
        protocol::group::GroupPacket r;
        if (!r.ParseFromArray(pkt.data() + sizeof(*hdr), hdr->body_len)) {
            ui::printf(ui::Color::Dim, "[client] <skip> bad group body");
            continue;
        }
        if (r.body_case() == expect) {
            resp = std::move(r);
            return true;
        }
        ui::printf(ui::Color::Dim, "[client] <skip> group case=%d", static_cast<int>(r.body_case()));
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
        ui::printf(ui::Color::Red, "[客户端] 发送失败");
        return false;
    }
    protocol::file::FilePacket::BodyCase expect = expected_file_resp_case(req);
    for (;;) {
        std::vector<char> pkt = pop_packet();
        if (pkt.empty()) {
            ui::printf(ui::Color::Red, "[客户端] 连接已断开");
            return false;
        }
        auto* hdr = reinterpret_cast<const protocol::packet_header*>(pkt.data());
        if (hdr->type != protocol::DOMAIN_FILE) {
            ui::printf(ui::Color::Dim, "[client] <skip> non-file domain=%u", hdr->type);
            continue;
        }
        protocol::file::FilePacket r;
        if (!r.ParseFromArray(pkt.data() + sizeof(*hdr), hdr->body_len)) {
            ui::printf(ui::Color::Dim, "[client] <skip> bad file body");
            continue;
        }
        if (r.body_case() == expect) {
            resp = std::move(r);
            return true;
        }
        ui::printf(ui::Color::Dim, "[client] <skip> file case=%d", static_cast<int>(r.body_case()));
    }
}

}  // namespace client

// ------------------------------------------------------------
//  命令行 UI
// ------------------------------------------------------------

namespace {

// readline 输入队列: on_line(回调)只入队, 主循环解锁后 process_queued_commands 执行
std::mutex  g_cmd_mtx;
std::deque<std::string> g_cmd_q;
bool g_eof = false;

// readline callback: 一行完整输入(含 Enter)后调用。不做任何打印(回调在持锁的
// rl_callback_read_char 内执行, 打印会死锁); 只入队 + 记历史。line==nullptr 表 EOF。
void on_line(char* line) {
    if (line == nullptr) {
        std::lock_guard<std::mutex> lk(g_cmd_mtx);
        g_eof = true;
        return;
    }
    if (line[0] != '\0' &&
        (history_length == 0 || strcmp(line, history_list()[history_length - 1]->line) != 0)) {
        add_history(line);   // 去重: 连续相同命令只记一次
    }
    {
        std::lock_guard<std::mutex> lk(g_cmd_mtx);
        g_cmd_q.emplace_back(line);
    }
    ::free(line);
}

// 执行排队命令; 返回 false 表示应退出(EOF 或命令 quit/exit)
bool process_queued_commands(ui::Session& s) {
    for (;;) {
        std::string line;
        {
            std::lock_guard<std::mutex> lk(g_cmd_mtx);
            if (g_eof && g_cmd_q.empty()) return false;
            if (g_cmd_q.empty()) return true;
            line = std::move(g_cmd_q.front());
            g_cmd_q.pop_front();
        }
        if (!s.handle(line)) return false;
    }
}

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

// 是否已知命令(去掉可选 '/' 前缀后匹配)。未知命令且设有会话目标时当作消息发送。
bool known_command(const char* cmd) {
    static const std::unordered_set<std::string> kSet = {
        "help", "quit", "exit", "register", "login", "logout", "cancel",
        "friend", "chat", "history",
        "gcreate", "gdissolve", "gjoin", "gapprove", "greject", "gpending",
        "gmembers", "gremove", "gpromote", "gquit", "glist", "gchat", "ghistory",
        "fsend", "fget", "flist", "fstat",
        "to", "g",
    };
    return kSet.count(cmd) != 0;
}

void print_help() {
    std::lock_guard<std::mutex> lk(ui::out_mtx());
    ui::write(ui::Color::Bold, "命令列表: <命令> [参数]   (命令均可加 '/' 前缀, 如 /help)\n");
    ui::write(ui::Color::Default, "\n");

    struct Sec { const char* title; const char* body; };
    static const Sec kSecs[] = {
        { "账号",
          "  register <用户名> <密码> <昵称>    注册\n"
          "  login    <用户名> <密码>           登录\n"
          "  logout                             登出\n"
          "  cancel   <用户名> <密码>           注销账号\n" },
        { "好友",
          "  friend req     <好友id> [申请信息]  发起加好友\n"
          "  friend pending                     查看待处理申请\n"
          "  friend list                        好友列表\n"
          "  friend del     <好友id>            删除好友\n"
          "  friend block   <好友id> [on|off]   拉黑 / 取消拉黑\n" },
        { "聊天",
          "  chat     <对方id> <消息>           私聊\n"
          "  history  <对方id> [个数]           私聊历史\n" },
        { "群组",
          "  gcreate    <群名>                  建群\n"
          "  gdissolve  <群id>                  解散群(群主)\n"
          "  gjoin      <群id> [申请消息]        申请入群\n"
          "  gapprove   <群id> <用户id>         批准入群(群主/管理员)\n"
          "  greject    <群id> <用户id>         拒绝入群(群主/管理员)\n"
          "  gpending   <群id>                  群待审批申请\n"
          "  gmembers   <群id>                  群成员列表\n"
          "  gremove    <群id> <用户id>         移除成员\n"
          "  gpromote   <群id> <用户id>         设为管理员\n"
          "  gquit      <群id>                  退群\n"
          "  glist                              我的群列表\n"
          "  gchat      <群id> <消息>           群聊\n"
          "  ghistory   <群id> [个数]           群聊历史\n" },
        { "会话模式(裸输入直接发送)",
          "  /to <用户id>                       选择私聊目标\n"
          "  /g  <群id>                         选择群目标\n"
          "  /to(无参)  /g(无参)                退出会话模式\n" },
        { "文件(上传目录 client/files/cli_send, 下载到 cli_recv)",
          "  fsend    <对方id> <文件名>         上传文件到对方收件箱\n"
          "  fget     <文件名>                  下载文件\n"
          "  flist                              我的收件箱文件列表\n"
          "  fstat    <文件名>                  查询服务器端文件大小\n" },
        { "其它",
          "  help                                本帮助\n"
          "  quit / exit                         退出\n" },
    };
    for (const auto& sec : kSecs) {
        ui::write(ui::Color::Bold, (std::string("  ── ") + sec.title + " ──\n").c_str());
        ui::write(ui::Color::Default, sec.body);
        ui::write(ui::Color::Default, "\n");
    }
    fflush(stdout);
}

// 表格打印: rows[0] 为表头(灰), 整块持锁原子输出, 避免推送插进表格中间
void print_table(const std::vector<std::string>& rows) {
    std::lock_guard<std::mutex> lk(ui::out_mtx());
    for (size_t i = 0; i < rows.size(); ++i) {
        ui::write(i == 0 ? ui::Color::Dim : ui::Color::Default, rows[i].c_str());
        fputc('\n', stdout);
    }
    fflush(stdout);
}

// 向服务端发送登出请求并等待响应; 返回是否登出成功。
// 连接已断开/请求失败时返回 false(退出流程不阻塞, 直接继续关闭)。
bool send_logout(int fd, uint32_t uid) {
    protocol::user::UserPacket req;
    req.mutable_logout_req()->set_user_id(uid);
    protocol::user::UserPacket resp;
    if (!client::user_request(fd, req, resp)) {
        ui::printf(ui::Color::Red, "[登出] 请求失败(连接已断开?)");
        return false;
    }
    const int err = resp.logout_resp().err();
    ui::printf(err == protocol::user::ERR_SUCCESS ? ui::Color::Green : ui::Color::Red,
               "[登出] err=%s(%d)", err_name(err), err);
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
        ui::printf(ui::Color::Red, "[发送文件] 文件名为空");
        return false;
    }
    std::string path = client_file_dir() + "/" + name;
    int lfd = ::open(path.c_str(), O_RDONLY);
    if (lfd < 0) {
        ui::printf(ui::Color::Red, "[发送文件] 无法打开 %s (请把文件放入 %s/)",
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
        ui::printf(ui::Color::Red, "[发送文件] err=%s(%d)", err_name(rr.err()),
                   static_cast<int>(rr.err()));
        ::close(lfd);
        return false;
    }
    uint64_t offset = rr.offset();
    if (offset > static_cast<uint64_t>(local_size)) offset = static_cast<uint64_t>(local_size);
    ::lseek(lfd, static_cast<off_t>(offset), SEEK_SET);
    ui::printf(ui::Color::Dim, "[发送文件] %s -> 用户 %u, 服务器偏移=%llu (从此续传)",
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
        if (cresp.chunk_ack().err() != protocol::user::ERR_SUCCESS) {
            ui::printf(ui::Color::Red, "[发送文件] 分片确认 err=%d",
                       static_cast<int>(cresp.chunk_ack().err()));
            ok = false;
            break;
        }
        offset += static_cast<uint64_t>(n);
    }
    ::close(lfd);
    if (!ok) {
        ui::printf(ui::Color::Red, "[发送文件] 中断于偏移 %llu (重跑 fsend 续传)",
                   static_cast<unsigned long long>(offset));
        return false;
    }

    protocol::file::FilePacket freq;
    freq.mutable_send_finish()->set_name(name);
    protocol::file::FilePacket fresp;
    if (!client::file_request(fd, freq, fresp)) return false;
    const auto& ack = fresp.send_ack();
    const bool ok2 = ack.err() == protocol::user::ERR_SUCCESS;
    ui::printf(ok2 ? ui::Color::Green : ui::Color::Red,
               "[发送文件] 完成 err=%s(%d) server_size=%llu",
               err_name(ack.err()), static_cast<int>(ack.err()),
               static_cast<unsigned long long>(ack.size()));
    return ok2;
}

// 从自己收件箱下载文件到 client/files/cli_recv/。断点续传逻辑仿 ftp handle_retr:
//   本地已有部分取其大小作 offset → 逐片 FileGetReq → 追加写入 → eof 结束。
bool download_file(int fd, const std::string& raw_name) {
    std::string name = client_basename(raw_name);
    if (name.empty()) {
        ui::printf(ui::Color::Red, "[下载文件] 文件名为空");
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
            ui::printf(ui::Color::Red, "[下载文件] err=%s(%d)", err_name(gr.err()),
                       static_cast<int>(gr.err()));
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
                    ui::printf(ui::Color::Red, "[下载文件] 无法创建 %s: %s",
                               path.c_str(), strerror(errno));
                    return false;
                }
            }
            size_t written = 0;
            while (written < data.size()) {
                ssize_t n = ::write(lfd, data.data() + written, data.size() - written);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                    ::close(lfd);
                    ui::printf(ui::Color::Red, "[下载文件] 写入失败");
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
        if (::truncate(path.c_str(), static_cast<off_t>(total)) != 0) {
            ui::printf(ui::Color::Red, "[下载文件] 截断 %s 失败: %s", path.c_str(), strerror(errno));
            return false;
        }
        offset = total;
    }
    ui::printf(ui::Color::Green, "[下载文件] %s 下载完成: %llu 字节", path.c_str(),
               static_cast<unsigned long long>(offset));
    return true;
}

// 会话模式下把裸输入发往当前目标(目标来自 /to /g 命令)
void session_send(ui::Session& s, const std::string& content) {
    if (!s.logged_in()) {
        ui::printf(ui::Color::Red, "[错误] 请先登录");
        return;
    }
    if (content.empty()) return;
    protocol::chat::ChatPacket req;
    auto* r = req.mutable_send_req();
    r->set_to_id(s.target_id);
    r->set_to_type(s.target_group ? protocol::chat::TARGET_TYPE_GROUP
                                  : protocol::chat::TARGET_TYPE_USER);
    r->set_content(content);
    protocol::chat::ChatPacket resp;
    if (!client::chat_request(s.fd, req, resp)) return;
    const auto& rr = resp.send_resp();
    if (rr.err() == protocol::user::ERR_SUCCESS) {
        std::string tname = s.target_group ? s.groups.display(s.target_id)
                                           : s.nick.display(s.target_id);
        ui::printf(ui::Color::Blue, "你 → %s: %s", tname.c_str(), content.c_str());
    } else {
        ui::printf(ui::Color::Red, "[发送失败] err=%s(%d)", err_name(rr.err()),
                   static_cast<int>(rr.err()));
    }
}

}  // namespace

// ------------------------------------------------------------
//  命令分发 —— 每条命令返回是否继续(false = 退出)
// ------------------------------------------------------------
bool ui::Session::handle(const std::string& line) {
    char raw[64] = {0};
    if (sscanf(line.c_str(), "%63s", raw) != 1) return true;

    // 去掉可选 '/' 前缀, 使 /help == help、/to == to
    std::string cmdstr = raw;
    if (!cmdstr.empty() && cmdstr[0] == '/') cmdstr = cmdstr.substr(1);
    const char* cmd = cmdstr.c_str();

    if (!known_command(cmd)) {
        if (has_target()) {
            session_send(*this, line);
            return true;
        }
        ui::printf(ui::Color::Red, "未知命令: %s (输入 'help' 查看帮助)", raw);
        return true;
    }

    if (strcmp(cmd, "help") == 0) {
        print_help();

    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        return false;

    } else if (strcmp(cmd, "to") == 0 || strcmp(cmd, "g") == 0) {
        const bool grp = (cmd[0] == 'g');
        if (!logged_in()) {
            ui::printf(ui::Color::Red, "[错误] 请先登录");
            return true;
        }
        uint32_t id = 0;
        if (sscanf(line.c_str(), "%*s %u", &id) == 1) {
            target_id = id;
            target_group = grp;
            std::string tname = grp ? groups.display(id) : nick.display(id);
            refresh_prompt();
            ui::printf(ui::Color::Green, "[目标] 已切换到%s %s(%u), 裸输入直接发送",
                       grp ? "群" : "用户", tname.c_str(), id);
        } else {
            target_id = 0;
            target_group = false;
            refresh_prompt();
            ui::printf(ui::Color::Green, "[目标] 已清除, 回到普通命令模式");
        }

    } else if (strcmp(cmd, "register") == 0) {
        char username[128] = {0}, password[128] = {0}, nickname[128] = {0};
        if (sscanf(line.c_str(), "%*s %127s %127s %127s", username, password, nickname) != 3) {
            ui::printf(ui::Color::Red, "用法: register <username> <password> <nickname>");
            return true;
        }
        protocol::user::UserPacket req;
        auto* r = req.mutable_register_req();
        r->set_username(username);
        r->set_password(password);
        r->set_nickname(nickname);
        protocol::user::UserPacket resp;
        if (client::user_request(fd, req, resp)) {
            const auto& rr = resp.register_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[注册] err=%s(%d) user_id=%u",
                       err_name(rr.err()), static_cast<int>(rr.err()), rr.user_id());
        }

    } else if (strcmp(cmd, "login") == 0) {
        char username[128] = {0}, password[128] = {0};
        if (sscanf(line.c_str(), "%*s %127s %127s", username, password) != 2) {
            ui::printf(ui::Color::Red, "用法: login <username> <password>");
            return true;
        }
        protocol::user::UserPacket req;
        auto* r = req.mutable_login_req();
        r->set_username(username);
        r->set_password(password);
        protocol::user::UserPacket resp;
        if (client::user_request(fd, req, resp)) {
            const auto& rr = resp.login_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[登录] err=%s(%d)", err_name(rr.err()), static_cast<int>(rr.err()));
            if (ok) {
                uid = rr.user_id();
                this->username = username;
                nickname = rr.user().nickname();
                nick.update(uid, nickname);
                refresh_prompt();   // prompt 显示 <昵称(uid)>
                ui::printf(ui::Color::Green, "  欢迎, %s (%u)", nickname.c_str(), uid);
            }
        }

    } else if (strcmp(cmd, "logout") == 0) {
        if (!logged_in()) {
            ui::printf(ui::Color::Red, "[错误] 请先登录");
        } else if (send_logout(fd, uid)) {
            uid = 0;
            username.clear();
            nickname.clear();
            target_id = 0;
            target_group = false;
            refresh_prompt();
        }

    } else if (strcmp(cmd, "cancel") == 0) {
        char username[128] = {0}, password[128] = {0};
        if (sscanf(line.c_str(), "%*s %127s %127s", username, password) != 2) {
            ui::printf(ui::Color::Red, "用法: cancel <username> <password>");
            return true;
        }
        protocol::user::UserPacket req;
        auto* r = req.mutable_cancel_req();
        r->set_username(username);
        r->set_password(password);
        protocol::user::UserPacket resp;
        if (client::user_request(fd, req, resp)) {
            const auto& rr = resp.cancel_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[注销] err=%s(%d)", err_name(rr.err()), static_cast<int>(rr.err()));
            // 注销的正是当前登录账号: 本地清空登录态(服务端已解绑)
            if (ok && logged_in() && strcmp(username, this->username.c_str()) == 0) {
                uid = 0;
                this->username.clear();
                nickname.clear();
                target_id = 0;
                target_group = false;
                refresh_prompt();
            }
        }

    } else if (strcmp(cmd, "friend") == 0) {
        char sub[64] = {0};
        if (sscanf(line.c_str(), "%*s %63s", sub) != 1) {
            ui::printf(ui::Color::Red, "用法: friend req|pending|list|del|block ...");
            return true;
        }
        if (strcmp(sub, "req") == 0) {
            uint32_t fid = 0;
            char remark[128] = {0};
            if (sscanf(line.c_str(), "%*s %*s %u %127[^\n]", &fid, remark) < 1) {
                ui::printf(ui::Color::Red, "用法: friend req <friend_id> [remark]");
                return true;
            }
            protocol::user::UserPacket req;
            auto* r = req.mutable_friend_request_req();
            r->set_friend_id(fid);
            r->set_remark(remark);
            protocol::user::UserPacket resp;
            if (client::user_request(fd, req, resp)) {
                const auto& rr = resp.friend_request_resp();
                const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
                ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                           "[好友申请] err=%s(%d) friend_id=%u",
                           err_name(rr.err()), static_cast<int>(rr.err()), rr.friend_id());
            }
        } else if (strcmp(sub, "pending") == 0) {
            protocol::user::UserPacket req;
            req.mutable_friend_pending_list_req();
            protocol::user::UserPacket resp;
            if (client::user_request(fd, req, resp)) {
                const auto& rr = resp.friend_pending_list_resp();
                const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
                ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                           "[待处理申请] err=%s(%d) items=%d",
                           err_name(rr.err()), static_cast<int>(rr.err()), rr.items_size());
                if (ok) {
                    std::vector<std::string> rows;
                    rows.push_back(ui::pad("ID", 10) + ui::pad("昵称", 20)
                                   + ui::pad("申请时间", 10) + "留言");
                    for (const auto& it : rr.items()) {
                        nick.update(it.friend_id(), it.nickname());
                        rows.push_back(ui::pad(std::to_string(it.friend_id()), 10)
                                       + ui::pad(it.nickname(), 20)
                                       + ui::pad(ui::fmt_ts(it.ts() * 1000), 10)
                                       + it.remark());
                    }
                    print_table(rows);
                }
            }
        } else if (strcmp(sub, "list") == 0) {
            protocol::user::UserPacket req;
            req.mutable_friend_list_req();
            protocol::user::UserPacket resp;
            if (client::user_request(fd, req, resp)) {
                const auto& rr = resp.friend_list_resp();
                const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
                ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                           "[好友列表] err=%s(%d) friends=%d",
                           err_name(rr.err()), static_cast<int>(rr.err()), rr.friends_size());
                if (ok) {
                    std::vector<std::string> rows;
                    rows.push_back(ui::pad("ID", 10) + ui::pad("昵称", 20) + "状态");
                    for (const auto& it : rr.friends()) {
                        nick.update(it.user_id(), it.nickname());
                        rows.push_back(ui::pad(std::to_string(it.user_id()), 10)
                                       + ui::pad(it.nickname(), 20)
                                       + (it.online() ? "在线" : "离线"));
                    }
                    print_table(rows);
                }
            }
        } else if (strcmp(sub, "del") == 0) {
            uint32_t fid = 0;
            if (sscanf(line.c_str(), "%*s %*s %u", &fid) != 1) {
                ui::printf(ui::Color::Red, "用法: friend del <friend_id>");
                return true;
            }
            protocol::user::UserPacket req;
            req.mutable_friend_del_req()->set_friend_id(fid);
            protocol::user::UserPacket resp;
            if (client::user_request(fd, req, resp)) {
                const auto& rr = resp.friend_del_resp();
                const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
                ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                           "[删除好友] err=%s(%d)", err_name(rr.err()), static_cast<int>(rr.err()));
            }
        } else if (strcmp(sub, "block") == 0) {
            uint32_t fid = 0;
            char mode[8] = {0};
            if (sscanf(line.c_str(), "%*s %*s %u %7s", &fid, mode) < 1) {
                ui::printf(ui::Color::Red, "用法: friend block <friend_id> [on|off]");
                return true;
            }
            bool block = (mode[0] == '\0') || (strcmp(mode, "on") == 0);
            protocol::user::UserPacket req;
            auto* r = req.mutable_friend_block_req();
            r->set_friend_id(fid);
            r->set_block(block);
            protocol::user::UserPacket resp;
            if (client::user_request(fd, req, resp)) {
                const auto& rr = resp.friend_block_resp();
                const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
                ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                           "[拉黑] err=%s(%d)", err_name(rr.err()), static_cast<int>(rr.err()));
            }
        } else {
            ui::printf(ui::Color::Red, "用法: friend req|pending|list|del|block ...");
        }

    } else if (strcmp(cmd, "chat") == 0) {
        uint32_t to_id = 0;
        char content[4096] = {0};
        if (sscanf(line.c_str(), "%*s %u %4095[^\n]", &to_id, content) < 1) {
            ui::printf(ui::Color::Red, "用法: chat <to_id> <text>");
            return true;
        }
        protocol::chat::ChatPacket req;
        auto* r = req.mutable_send_req();
        r->set_to_id(to_id);
        r->set_content(content);
        protocol::chat::ChatPacket resp;
        if (client::chat_request(fd, req, resp)) {
            const auto& rr = resp.send_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[聊天] err=%s(%d) msg_id=%llu server_ts=%llu",
                       err_name(rr.err()), static_cast<int>(rr.err()),
                       static_cast<unsigned long long>(rr.msg_id()),
                       static_cast<unsigned long long>(rr.server_ts()));
        }

    } else if (strcmp(cmd, "history") == 0) {
        uint32_t target_id = 0;
        uint32_t limit = 50;
        int n = sscanf(line.c_str(), "%*s %u %u", &target_id, &limit);
        if (n < 1) {
            ui::printf(ui::Color::Red, "用法: history <target_id> [limit]");
            return true;
        }
        protocol::chat::ChatPacket req;
        auto* r = req.mutable_history_req();
        r->set_target_id(target_id);
        r->set_after_msg_id(0);
        r->set_limit(limit);
        protocol::chat::ChatPacket resp;
        if (client::chat_request(fd, req, resp)) {
            const auto& rr = resp.history_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[历史消息] err=%s(%d) messages=%d",
                       err_name(rr.err()), static_cast<int>(rr.err()), rr.messages_size());
            if (ok) {
                std::vector<std::string> rows;
                rows.push_back(ui::pad("ID", 6) + ui::pad("方向", 8)
                               + ui::pad("时间", 10) + "内容");
                for (const auto& m : rr.messages()) {
                    rows.push_back(ui::pad(std::to_string(m.msg_id()), 6)
                                   + ui::pad(m.from_id() == uid ? "我" : "对方", 8)
                                   + ui::pad(ui::fmt_ts(m.ts()), 10) + m.content());
                }
                print_table(rows);
            }
        }

    } else if (strcmp(cmd, "gcreate") == 0) {
        char name[64] = {0};
        if (sscanf(line.c_str(), "%*s %63s", name) != 1) {
            ui::printf(ui::Color::Red, "用法: gcreate <name>");
            return true;
        }
        protocol::group::GroupPacket req;
        req.mutable_create_req()->set_name(name);
        protocol::group::GroupPacket resp;
        if (client::group_request(fd, req, resp)) {
            const auto& rr = resp.create_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[建群] err=%s(%d) group_id=%u",
                       err_name(rr.err()), static_cast<int>(rr.err()), rr.group_id());
            if (ok) groups.update(rr.group_id(), name);
        }

    } else if (strcmp(cmd, "gdissolve") == 0) {
        uint32_t gid = 0;
        if (sscanf(line.c_str(), "%*s %u", &gid) != 1) {
            ui::printf(ui::Color::Red, "用法: gdissolve <group_id>");
            return true;
        }
        protocol::group::GroupPacket req;
        req.mutable_dissolve_req()->set_group_id(gid);
        protocol::group::GroupPacket resp;
        if (client::group_request(fd, req, resp)) {
            const auto& rr = resp.dissolve_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[解散群] err=%s(%d) group_id=%u",
                       err_name(rr.err()), static_cast<int>(rr.err()), rr.group_id());
        }

    } else if (strcmp(cmd, "gjoin") == 0) {
        uint32_t gid = 0;
        char remark[128] = {0};
        if (sscanf(line.c_str(), "%*s %u %127[^\n]", &gid, remark) < 1) {
            ui::printf(ui::Color::Red, "用法: gjoin <group_id> [remark]");
            return true;
        }
        protocol::group::GroupPacket req;
        auto* r = req.mutable_join_req();
        r->set_group_id(gid);
        r->set_remark(remark);
        protocol::group::GroupPacket resp;
        if (client::group_request(fd, req, resp)) {
            const auto& rr = resp.join_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[入群] err=%s(%d) group_id=%u",
                       err_name(rr.err()), static_cast<int>(rr.err()), rr.group_id());
        }

    } else if (strcmp(cmd, "gapprove") == 0) {
        uint32_t gid = 0, uid_arg = 0;
        if (sscanf(line.c_str(), "%*s %u %u", &gid, &uid_arg) != 2) {
            ui::printf(ui::Color::Red, "用法: gapprove <group_id> <user_id>");
            return true;
        }
        protocol::group::GroupPacket req;
        auto* r = req.mutable_approve_join_req();
        r->set_group_id(gid);
        r->set_user_id(uid_arg);
        protocol::group::GroupPacket resp;
        if (client::group_request(fd, req, resp)) {
            const auto& rr = resp.approve_join_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[批准入群] err=%s(%d) group_id=%u user_id=%u",
                       err_name(rr.err()), static_cast<int>(rr.err()),
                       rr.group_id(), rr.user_id());
        }

    } else if (strcmp(cmd, "greject") == 0) {
        uint32_t gid = 0, uid_arg = 0;
        if (sscanf(line.c_str(), "%*s %u %u", &gid, &uid_arg) != 2) {
            ui::printf(ui::Color::Red, "用法: greject <group_id> <user_id>");
            return true;
        }
        protocol::group::GroupPacket req;
        auto* r = req.mutable_reject_join_req();
        r->set_group_id(gid);
        r->set_user_id(uid_arg);
        protocol::group::GroupPacket resp;
        if (client::group_request(fd, req, resp)) {
            const auto& rr = resp.reject_join_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[拒绝入群] err=%s(%d) group_id=%u user_id=%u",
                       err_name(rr.err()), static_cast<int>(rr.err()),
                       rr.group_id(), rr.user_id());
        }

    } else if (strcmp(cmd, "gpending") == 0) {
        uint32_t gid = 0;
        if (sscanf(line.c_str(), "%*s %u", &gid) != 1) {
            ui::printf(ui::Color::Red, "用法: gpending <group_id>");
            return true;
        }
        protocol::group::GroupPacket req;
        req.mutable_pending_list_req()->set_group_id(gid);
        protocol::group::GroupPacket resp;
        if (client::group_request(fd, req, resp)) {
            const auto& rr = resp.pending_list_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[群申请] err=%s(%d) items=%d",
                       err_name(rr.err()), static_cast<int>(rr.err()), rr.items_size());
            if (ok) {
                std::vector<std::string> rows;
                rows.push_back(ui::pad("ID", 10) + ui::pad("昵称", 20)
                               + ui::pad("申请时间", 10) + "留言");
                for (const auto& it : rr.items()) {
                    nick.update(it.user_id(), it.nickname());
                    rows.push_back(ui::pad(std::to_string(it.user_id()), 10)
                                   + ui::pad(it.nickname(), 20)
                                   + ui::pad(ui::fmt_ts(it.ts() * 1000), 10)
                                   + it.remark());
                }
                print_table(rows);
            }
        }

    } else if (strcmp(cmd, "gmembers") == 0) {
        uint32_t gid = 0;
        if (sscanf(line.c_str(), "%*s %u", &gid) != 1) {
            ui::printf(ui::Color::Red, "用法: gmembers <group_id>");
            return true;
        }
        protocol::group::GroupPacket req;
        req.mutable_member_list_req()->set_group_id(gid);
        protocol::group::GroupPacket resp;
        if (client::group_request(fd, req, resp)) {
            const auto& rr = resp.member_list_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[群成员] err=%s(%d) members=%d",
                       err_name(rr.err()), static_cast<int>(rr.err()), rr.members_size());
            if (ok) {
                std::vector<std::string> rows;
                rows.push_back(ui::pad("ID", 10) + ui::pad("昵称", 20) + "角色");
                for (const auto& it : rr.members()) {
                    nick.update(it.user_id(), it.nickname());
                    rows.push_back(ui::pad(std::to_string(it.user_id()), 10)
                                   + ui::pad(it.nickname(), 20)
                                   + ui::role_name(it.role()));
                }
                print_table(rows);
            }
        }

    } else if (strcmp(cmd, "gremove") == 0) {
        uint32_t gid = 0, uid_arg = 0;
        if (sscanf(line.c_str(), "%*s %u %u", &gid, &uid_arg) != 2) {
            ui::printf(ui::Color::Red, "用法: gremove <group_id> <user_id>");
            return true;
        }
        protocol::group::GroupPacket req;
        auto* r = req.mutable_remove_member_req();
        r->set_group_id(gid);
        r->set_user_id(uid_arg);
        protocol::group::GroupPacket resp;
        if (client::group_request(fd, req, resp)) {
            const auto& rr = resp.remove_member_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[移除成员] err=%s(%d) group_id=%u user_id=%u",
                       err_name(rr.err()), static_cast<int>(rr.err()),
                       rr.group_id(), rr.user_id());
        }

    } else if (strcmp(cmd, "gpromote") == 0) {
        uint32_t gid = 0, uid_arg = 0;
        if (sscanf(line.c_str(), "%*s %u %u", &gid, &uid_arg) != 2) {
            ui::printf(ui::Color::Red, "用法: gpromote <group_id> <user_id>");
            return true;
        }
        protocol::group::GroupPacket req;
        auto* r = req.mutable_promote_admin_req();
        r->set_group_id(gid);
        r->set_user_id(uid_arg);
        protocol::group::GroupPacket resp;
        if (client::group_request(fd, req, resp)) {
            const auto& rr = resp.promote_admin_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[设为管理员] err=%s(%d) group_id=%u user_id=%u",
                       err_name(rr.err()), static_cast<int>(rr.err()),
                       rr.group_id(), rr.user_id());
        }

    } else if (strcmp(cmd, "gquit") == 0) {
        uint32_t gid = 0;
        if (sscanf(line.c_str(), "%*s %u", &gid) != 1) {
            ui::printf(ui::Color::Red, "用法: gquit <group_id>");
            return true;
        }
        protocol::group::GroupPacket req;
        req.mutable_quit_req()->set_group_id(gid);
        protocol::group::GroupPacket resp;
        if (client::group_request(fd, req, resp)) {
            const auto& rr = resp.quit_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[退群] err=%s(%d) group_id=%u",
                       err_name(rr.err()), static_cast<int>(rr.err()), rr.group_id());
        }

    } else if (strcmp(cmd, "glist") == 0) {
        protocol::group::GroupPacket req;
        req.mutable_group_list_req();
        protocol::group::GroupPacket resp;
        if (client::group_request(fd, req, resp)) {
            const auto& rr = resp.group_list_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[我的群] err=%s(%d) groups=%d",
                       err_name(rr.err()), static_cast<int>(rr.err()), rr.groups_size());
            if (ok) {
                std::vector<std::string> rows;
                rows.push_back(ui::pad("群ID", 10) + ui::pad("群名", 20) + "角色");
                for (const auto& it : rr.groups()) {
                    groups.update(it.group_id(), it.name());
                    rows.push_back(ui::pad(std::to_string(it.group_id()), 10)
                                   + ui::pad(it.name(), 20)
                                   + ui::role_name(it.role()));
                }
                print_table(rows);
            }
        }

    } else if (strcmp(cmd, "gchat") == 0) {
        uint32_t gid = 0;
        char content[4096] = {0};
        if (sscanf(line.c_str(), "%*s %u %4095[^\n]", &gid, content) < 1) {
            ui::printf(ui::Color::Red, "用法: gchat <group_id> <text>");
            return true;
        }
        protocol::chat::ChatPacket req;
        auto* r = req.mutable_send_req();
        r->set_to_id(gid);
        r->set_to_type(protocol::chat::TARGET_TYPE_GROUP);
        r->set_content(content);
        protocol::chat::ChatPacket resp;
        if (client::chat_request(fd, req, resp)) {
            const auto& rr = resp.send_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[群聊] err=%s(%d) msg_id=%llu server_ts=%llu",
                       err_name(rr.err()), static_cast<int>(rr.err()),
                       static_cast<unsigned long long>(rr.msg_id()),
                       static_cast<unsigned long long>(rr.server_ts()));
        }

    } else if (strcmp(cmd, "ghistory") == 0) {
        uint32_t gid = 0;
        uint32_t limit = 50;
        int n = sscanf(line.c_str(), "%*s %u %u", &gid, &limit);
        if (n < 1) {
            ui::printf(ui::Color::Red, "用法: ghistory <group_id> [limit]");
            return true;
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
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[群历史] err=%s(%d) messages=%d",
                       err_name(rr.err()), static_cast<int>(rr.err()), rr.messages_size());
            if (ok) {
                std::vector<std::string> rows;
                rows.push_back(ui::pad("ID", 6) + ui::pad("成员", 8)
                               + ui::pad("时间", 10) + "内容");
                for (const auto& m : rr.messages()) {
                    rows.push_back(ui::pad(std::to_string(m.msg_id()), 6)
                                   + ui::pad(m.from_id() == uid ? "我" : "成员", 8)
                                   + ui::pad(ui::fmt_ts(m.ts()), 10) + m.content());
                }
                print_table(rows);
            }
        }

    } else if (strcmp(cmd, "fsend") == 0) {
        uint32_t to_id = 0;
        char fname[128] = {0};
        if (sscanf(line.c_str(), "%*s %u %127s", &to_id, fname) != 2) {
            ui::printf(ui::Color::Red, "用法: fsend <to_id> <filename>   (从 %s/ 目录中查找)",
                       client_file_dir().c_str());
            return true;
        }
        if (!logged_in()) {
            ui::printf(ui::Color::Red, "[错误] 请先登录");
            return true;
        }
        upload_file(fd, to_id, fname);

    } else if (strcmp(cmd, "fget") == 0) {
        char fname[128] = {0};
        if (sscanf(line.c_str(), "%*s %127s", fname) != 1) {
            ui::printf(ui::Color::Red, "用法: fget <filename>");
            return true;
        }
        if (!logged_in()) {
            ui::printf(ui::Color::Red, "[错误] 请先登录");
            return true;
        }
        download_file(fd, fname);

    } else if (strcmp(cmd, "flist") == 0) {
        if (!logged_in()) {
            ui::printf(ui::Color::Red, "[错误] 请先登录");
            return true;
        }
        protocol::file::FilePacket req;
        req.mutable_list_req()->set_owner_id(uid);
        protocol::file::FilePacket resp;
        if (client::file_request(fd, req, resp)) {
            const auto& rr = resp.list_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[文件列表] err=%s(%d) files=%d",
                       err_name(rr.err()), static_cast<int>(rr.err()), rr.files_size());
            if (ok) {
                std::vector<std::string> rows;
                rows.push_back(ui::pad("文件名", 30) + "大小");
                for (const auto& f : rr.files()) {
                    rows.push_back(ui::pad(f.name(), 30)
                                   + std::to_string(f.size()) + " 字节");
                }
                print_table(rows);
            }
        }

    } else if (strcmp(cmd, "fstat") == 0) {
        char fname[128] = {0};
        if (sscanf(line.c_str(), "%*s %127s", fname) != 1) {
            ui::printf(ui::Color::Red, "用法: fstat <filename>");
            return true;
        }
        if (!logged_in()) {
            ui::printf(ui::Color::Red, "[错误] 请先登录");
            return true;
        }
        protocol::file::FilePacket req;
        req.mutable_stat_req()->set_name(fname);
        protocol::file::FilePacket resp;
        if (client::file_request(fd, req, resp)) {
            const auto& rr = resp.stat_resp();
            const bool ok = rr.err() == protocol::user::ERR_SUCCESS;
            ui::printf(ok ? ui::Color::Green : ui::Color::Red,
                       "[文件大小] err=%s(%d) size=%llu",
                       err_name(rr.err()), static_cast<int>(rr.err()),
                       static_cast<unsigned long long>(rr.size()));
        }

    } else {
        ui::printf(ui::Color::Red, "未知命令: %s (输入 'help' 查看帮助)", raw);
    }
    return true;
}

int main(int argc, char* argv[]) {
    // 忽略 SIGPIPE: TLS 路径的 SSL_write 可能向已断开的 socket 写(如连接被拒后
    // 仍做握手), 默认会 SIGPIPE 杀死进程并丢掉未刷新的 stdout。忽略后返回 EPIPE, 能干净报错。
    signal(SIGPIPE, SIG_IGN);

    // 仅提示文件目录位置(目录为仓库内已存在的 cli_send/cli_recv, 代码不创建)。
    ui::printf(ui::Color::Dim, "[客户端] 文件目录: 上传 %s/  下载 %s/ (把要发的文件放入上传目录)",
               client_file_dir().c_str(), client_recv_dir().c_str());

    const char* ip = "127.0.0.1";
    int port = 2100;
    if (argc > 1) ip = argv[1];
    if (argc > 2) port = std::atoi(argv[2]);

    network::Socket sock;
    if (sock.connect(ip, port) != 0) {
        ui::printf(ui::Color::Red, "[客户端] 连接 %s:%d 失败", ip, port);
        return 1;
    }
    int fd = sock.fd();

    // ---- TLS 握手(握手期间保持阻塞; 完成后转非阻塞 + 互斥锁并发读写) ----
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
        ui::printf(ui::Color::Red, "[client] SSL_CTX_new failed");
        return 1;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);
    // 开发环境跳过自签名证书校验。生产环境应改为 SSL_VERIFY_PEER +
    // SSL_CTX_load_verify_locations(ca) 并检查 SSL_get_verify_result。
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    SSL* ssl = SSL_new(ctx);
    if (ssl == nullptr) {
        ui::printf(ui::Color::Red, "[client] SSL_new failed");
        SSL_CTX_free(ctx);
        return 1;
    }
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) {
        ui::printf(ui::Color::Red, "[client] TLS handshake failed");
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return 1;
    }
    g_ssl = ssl;
    sock.set_nonblock();   // 此后由 poll 驱动等待, 避免持锁阻塞其它方向的 SSL 操作
    ui::printf(ui::Color::Green, "[客户端] 已连接 %s:%d (fd=%d) tls=%s",
               ip, port, fd, SSL_get_version(ssl));
    print_help();

    ui::Session session;
    session.fd = fd;
    ui::g_session = &session;   // reader 线程打印推送时经此取昵称

    client::start_reader(fd);   // 后台线程常驻收包, 推送实时打印
    client::start_heartbeat(fd);  // 后台线程周期发心跳, 保持连接活性

    if (ui::tty_io()) {
        // readline callback 模式: 与 poll 循环集成(200ms 轮询以便感知 g_closed),
        // 有输入才 rl_callback_read_char(它内部阻塞读 stdin, 无输入调用会卡住)。
        rl_callback_handler_install(session.prompt().c_str(), on_line);
        rl_attempted_completion_function = ui::complete;
        ui::set_rl_active(true);
        while (!client::g_closed) {
            struct pollfd pfd;
            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = ::poll(&pfd, 1, 200);
            if (pr < 0 && errno == EINTR) continue;
            if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
                // 持 g_out_mtx 调 readline, 与 reader 线程的 rl_redisplay 互斥
                std::lock_guard<std::mutex> lk(ui::out_mtx());
                rl_callback_read_char();
            }
            if (!process_queued_commands(session)) break;
        }
        ui::set_rl_active(false);
        rl_callback_handler_remove();   // 恢复终端
    } else {
        // 非 tty(管道/脚本输入): fgets 回退, 无行编辑/补全/重绘
        std::string line;
        while (!client::g_closed && std::getline(std::cin, line)) {
            if (!session.handle(line)) break;
        }
    }

    // 结束输入行(占位), 后续清理输出另起一行, 避免贴在 readline 提示符后
    ui::printf(ui::Color::Default, "");
    if (client::g_closed) {
        ui::printf(ui::Color::Yellow, "[客户端] 与服务器的连接已断开");
    }

    // 退出前若已登录, 先向服务端登出(online 置 0, 并向好友推送下线通知)。
    // 覆盖 quit / exit / EOF 三种退出路径。
    if (session.uid != 0) {
        send_logout(fd, session.uid);
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
    ui::printf(ui::Color::Dim, "[客户端] 会话已结束.");
    return 0;
}
