#include "client.hpp"

#include "../network/socket/socket.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>

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

// 阻塞读满 n 字节, 成功返回 true
bool recv_exact(int fd, char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r > 0) {
            got += static_cast<size_t>(r);
        } else if (r == 0) {
            return false;  // 对端关闭
        } else if (errno == EINTR) {
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
        ssize_t r = ::send(fd, data + sent, n - sent, MSG_NOSIGNAL);
        if (r > 0) {
            sent += static_cast<size_t>(r);
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(1000);  // 发送缓冲区暂满, 稍后重试
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
        case P::kFriendDelReq:         return P::kFriendDelResp;
        case P::kFriendCheckReq:       return P::kFriendCheckResp;
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
                fprintf(stdout, "\n[chat<<] from=%u: %s\n", m.from_id(), m.content().c_str());
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
            "  friend del     <friend_id>\n"
            "  friend check   <friend_id>\n"
            "  friend block   <friend_id> [on|off]\n"
            "  chat     <to_id> <text>\n"
            "  history  <target_id> [limit]\n"
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

}  // namespace

int main(int argc, char* argv[]) {
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
    fprintf(stdout, "[client] connected to %s:%d (fd=%d)\n", ip, port, fd);
    print_help();

    client::start_reader(fd);   // 后台线程常驻收包, 推送实时打印
    client::start_heartbeat(fd);  // 后台线程周期发心跳, 保持连接活性

    uint32_t g_uid = 0;        // 当前登录用户 id
    char g_username[128] = {0};  // 当前登录用户名(用于判断注销的是否为当前账号)

    char line[1024];
    while (1) {
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
                fprintf(stderr, "usage: friend req|pending|del|check|block ...\n");
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
            } else if (strcmp(sub, "check") == 0) {
                uint32_t fid = 0;
                if (sscanf(line, "%*s %*s %u", &fid) != 1) {
                    fprintf(stderr, "usage: friend check <friend_id>\n");
                    continue;
                }
                protocol::user::UserPacket req;
                req.mutable_friend_check_req()->set_friend_id(fid);
                protocol::user::UserPacket resp;
                if (client::user_request(fd, req, resp)) {
                    const auto& rr = resp.friend_check_resp();
                    fprintf(stdout, "[friend.check] err=%s(%d) is_friend=%d nickname='%s'\n",
                            err_name(rr.err()), static_cast<int>(rr.err()),
                            rr.is_friend() ? 1 : 0, rr.nickname().c_str());
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
                fprintf(stderr, "usage: friend req|pending|del|check|block ...\n");
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

        } else {
            fprintf(stderr, "unknown command: %s (try 'help')\n", cmd);
        }
    }

    // 退出前若已登录, 先向服务端登出(online 置 0, 并向好友推送下线通知)。
    // 覆盖 quit / exit / EOF 三种退出路径。
    if (g_uid != 0) {
        send_logout(fd, g_uid);
    }

    ::close(fd);
    fprintf(stdout, "\n[client] closed.\n");
    return 0;
}
