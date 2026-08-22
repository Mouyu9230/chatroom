#include "ui.hpp"

#include <readline/readline.h>
#include <readline/history.h>

#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace ui {

// ------------------------------------------------------------
//  输出层
// ------------------------------------------------------------
std::mutex& out_mtx() {
    static std::mutex m;
    return m;
}

static std::atomic<bool> g_rl_active{false};
bool rl_active() { return g_rl_active.load(); }
void set_rl_active(bool on) { g_rl_active.store(on); }

bool use_color() {
    // 一次判定: stdout 是终端且未设 NO_COLOR → 输出 ANSI。管道/重定向自动降级纯文本。
    static const bool v = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == nullptr;
    return v;
}

bool tty_io() {
    // readline 需要真正的终端才能做行编辑; stdin/stdout 任一被重定向即回退 fgets。
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

static const char* ansi(Color c) {
    switch (c) {
        case Color::Red:    return "\x1b[31m";
        case Color::Green:  return "\x1b[32m";
        case Color::Yellow: return "\x1b[33m";
        case Color::Blue:   return "\x1b[34m";
        case Color::Cyan:   return "\x1b[36m";
        case Color::Dim:    return "\x1b[2m";
        case Color::Bold:   return "\x1b[1m";
        default:            return "";
    }
}
static const char* kReset = "\x1b[0m";

void write(Color c, const char* s) {
    // 假定调用者已持 out_mtx()
    if (use_color() && c != Color::Default) fputs(ansi(c), stdout);
    fputs(s, stdout);
    if (use_color() && c != Color::Default) fputs(kReset, stdout);
}

static std::string vformat(const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string s;
    if (n > 0) {
        s.resize(static_cast<size_t>(n));
        vsnprintf(&s[0], static_cast<size_t>(n) + 1, fmt, ap2);
    }
    va_end(ap2);
    return s;
}

void printf(Color c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = vformat(fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(out_mtx());
    write(c, s.c_str());
    fputc('\n', stdout);
    fflush(stdout);
}

void puts(Color c, const std::string& s) {
    std::lock_guard<std::mutex> lk(out_mtx());
    write(c, s.c_str());
    fputc('\n', stdout);
    fflush(stdout);
}

void push_printf(Color c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = vformat(fmt, ap);
    va_end(ap);
    push_puts(c, s);
}

void push_puts(Color c, const std::string& text) {
    // reader/心跳线程: 擦掉正在输入的行 → 打印推送 → 重绘 prompt+输入缓冲。
    // 重绘仅在 stdout 是终端时才有意义(tty_io 保证 readline 只在 tty 下启用)。
    std::lock_guard<std::mutex> lk(out_mtx());
    if (rl_active() && use_color()) fputs("\r\x1b[K", stdout);   // erase current line
    write(c, text.c_str());
    fputc('\n', stdout);
    if (rl_active() && use_color()) {
        // readline 增量重绘: 它不感知我们刚擦掉的那行, 状态没变就不重发。
        // rl_on_new_line() 强制它把整行(含 prompt+输入缓冲)当"新的一行"全量重绘。
        rl_on_new_line();
        rl_redisplay();
    }
    fflush(stdout);
}

// ------------------------------------------------------------
//  格式化
// ------------------------------------------------------------
std::string fmt_ts(uint64_t ms) {
    time_t sec = static_cast<time_t>(ms / 1000);
    struct tm tmv;
    localtime_r(&sec, &tmv);
    char b[32];
    strftime(b, sizeof b, "%H:%M:%S", &tmv);
    return b;
}

const char* role_name(int role) {
    switch (role) {
        case 1: return "群主";
        case 2: return "管理员";
        case 3: return "成员";
        default: return "?";
    }
}

// 逐个解码 UTF-8 码点, 估算显示宽度。用码点 >= 0x1100 近似全角字符(中文/日文/韩文/全角标点),
// 不引入 locale 依赖(设置全局 locale 会影响进程其它格式化输出)。
static uint32_t decode_cp(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { i += 1; return c; }
    if ((c >> 5) == 0x6 && i + 1 < s.size()) {
        uint32_t cp = ((c & 0x1f) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3f);
        i += 2;
        return cp;
    }
    if ((c >> 4) == 0xe && i + 2 < s.size()) {
        uint32_t cp = ((c & 0x0f) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3f) << 6)
                    | (static_cast<unsigned char>(s[i + 2]) & 0x3f);
        i += 3;
        return cp;
    }
    if ((c >> 3) == 0x1e && i + 3 < s.size()) {
        uint32_t cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3f) << 12)
                    | ((static_cast<unsigned char>(s[i + 2]) & 0x3f) << 6)
                    | (static_cast<unsigned char>(s[i + 3]) & 0x3f);
        i += 4;
        return cp;
    }
    i += 1;
    return c;
}

int display_width(const std::string& s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = decode_cp(s, i);
        w += (cp >= 0x1100) ? 2 : 1;
    }
    return w;
}

std::string pad(const std::string& s, int width) {
    std::string r = s;
    int w = display_width(s);
    for (int i = w; i < width; ++i) r += ' ';
    return r;
}

// ------------------------------------------------------------
//  昵称 / 群名缓存
// ------------------------------------------------------------
void NickCache::update(uint32_t uid, const std::string& name) {
    if (name.empty()) return;
    std::lock_guard<std::mutex> lk(m_mtx_);
    map_[uid] = name;
}
bool NickCache::has(uint32_t uid) const {
    std::lock_guard<std::mutex> lk(m_mtx_);
    return map_.count(uid) != 0;
}
std::string NickCache::display(uint32_t uid) const {
    std::lock_guard<std::mutex> lk(m_mtx_);
    auto it = map_.find(uid);
    return it == map_.end() ? std::to_string(uid) : it->second;
}

void GroupCache::update(uint32_t gid, const std::string& name) {
    if (name.empty()) return;
    std::lock_guard<std::mutex> lk(m_mtx_);
    map_[gid] = name;
}
bool GroupCache::has(uint32_t gid) const {
    std::lock_guard<std::mutex> lk(m_mtx_);
    return map_.count(gid) != 0;
}
std::string GroupCache::display(uint32_t gid) const {
    std::lock_guard<std::mutex> lk(m_mtx_);
    auto it = map_.find(gid);
    return it == map_.end() ? std::to_string(gid) : it->second;
}

Session* g_session = nullptr;

// ------------------------------------------------------------
//  Session: prompt / 刷新
// ------------------------------------------------------------
std::string Session::prompt() const {
    // readline 计算光标位置时会把 \001..\002 间的不可见序列当宽度 0 —— 色码必须包裹
    const std::string B = "\001\x1b[1;36m\002";   // bold cyan
    const std::string Y = "\001\x1b[1;33m\002";   // bold yellow
    const std::string R = "\001\x1b[0m\002";
    std::string p;
    if (logged_in()) {
        std::string id = nickname.empty() ? std::to_string(uid)
                                          : nickname + "(" + std::to_string(uid) + ")";
        if (use_color()) p += B + id + R + " ";
        else p += "<" + id + "> ";
    }
    if (has_target()) {
        std::string name = target_group ? groups.display(target_id) : nick.display(target_id);
        std::string t = "[→ " + name + (target_group ? " 群]" : "]");
        if (use_color()) p += Y + t + R + " ";
        else p += t + " ";
    }
    p += "client> ";
    return p;
}

void Session::refresh_prompt() {
    // 主线程刷新 prompt; 持锁避免与 reader 线程的 rl_redisplay 竞争。
    std::lock_guard<std::mutex> lk(out_mtx());
    if (rl_active()) {
        std::string p = prompt();
        rl_set_prompt(p.c_str());
        rl_redisplay();
    }
}

// ------------------------------------------------------------
//  readline Tab 补全
// ------------------------------------------------------------
char** complete(const char* text, int start, int end) {
    (void)end;
    rl_attempted_completion_over = 1;
    std::vector<std::string> cands;

    if (start == 0) {
        static const char* kCommands[] = {
            "help", "quit", "exit", "register", "login", "logout", "cancel",
            "friend", "chat", "history",
            "gcreate", "gdissolve", "gjoin", "gapprove", "greject", "gpending",
            "gmembers", "gremove", "gpromote", "gquit", "glist", "gchat", "ghistory",
            "fsend", "fget", "flist", "fstat",
            "to", "g",
        };
        // 用户可能已敲了 '/' 前缀: 剥离后按命令名匹配, 补全时加回 '/'
        bool slash = (text[0] == '/');
        const char* t = text;
        char strip[128];
        if (slash) {
            snprintf(strip, sizeof strip, "%s", text + 1);
            t = strip;
        }
        size_t n = strlen(t);
        for (auto* c : kCommands)
            if (strncmp(c, t, n) == 0)
                cands.push_back(slash ? "/" + std::string(c) : std::string(c));
    } else {
        // 第二词: "friend" 子命令
        char first[64] = {0};
        sscanf(rl_line_buffer, "%63s", first);
        if (strcmp(first, "friend") == 0 || strcmp(first, "/friend") == 0) {
            static const char* kSub[] = {"req", "pending", "list", "del", "block"};
            size_t n = strlen(text);
            for (auto* c : kSub)
                if (strncmp(c, text, n) == 0) cands.push_back(c);
        }
    }

    if (cands.empty()) return nullptr;
    char** res = static_cast<char**>(malloc((cands.size() + 1) * sizeof(char*)));
    for (size_t i = 0; i < cands.size(); ++i) res[i] = strdup(cands[i].c_str());
    res[cands.size()] = nullptr;
    return res;
}

}  // namespace ui
