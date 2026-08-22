#pragma once

// ============================================================
//  client/ui.hpp —— CLI 客户端 UI 层
//
//  定位: 输出层(语义色 + 线程安全的推送打印)、格式化工具、
//        昵称/群名缓存、会话状态(Session)、readline 胶水。
//
//  线程模型:
//    - 主线程:  命令分发(Session::handle)、readline callback 处理
//    - reader 线程: 收包推送 → ui::push_* (持 g_out_mtx 擦行重绘)
//    - 心跳线程: 报错 → 同 push 路径
//    所有 stdout 写都经 ui::out_mtx() 互斥; readline 内部状态只在持锁时被触及。
// ============================================================

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ui {

// ------------------------------------------------------------
//  语义色
// ------------------------------------------------------------
enum class Color {
    Default,   // 无
    Dim,       // 灰: 元数据/说明
    Red,       // 错误
    Green,     // 成功
    Yellow,    // 系统/警告
    Cyan,      // 收到的消息
    Blue,      // 自己发出的消息
    Bold,      // 强调
};

// ------------------------------------------------------------
//  输出层 —— 所有 stdout 写统一经 out_mtx() 互斥
// ------------------------------------------------------------
std::mutex& out_mtx();

/// 是否使用 ANSI 色: isatty(stdout) 且未设 NO_COLOR
bool use_color();
/// stdin/stdout 是否都是终端(决定 readline 还是 fgets 回退)
bool tty_io();
/// reader 线程(以及 fgets 回退路径)是否处于活动状态
bool rl_active();
void set_rl_active(bool on);

/// 以指定颜色写一段文本(假定调用者已持 out_mtx)
void write(Color c, const char* s);
/// 锁定并打印一行(自动加 \n)
void printf(Color c, const char* fmt, ...);
/// 锁定并打印一段纯文本(自动加 \n)
void puts(Color c, const std::string& s);

/// reader/心跳线程打印一行推送: 擦掉当前输入行 → 打印 → 重绘 prompt+输入缓冲。
/// 内部持锁, 线程安全。仅在 tty 下重绘; 非 tty 退化为普通打印。
void push_printf(Color c, const char* fmt, ...);
void push_puts(Color c, const std::string& text);

// ------------------------------------------------------------
//  格式化工具
// ------------------------------------------------------------
/// 毫秒时间戳 → 本地 "HH:MM:SS"
std::string fmt_ts(uint64_t ms);
/// group 域 GroupRole 数字 → "群主/管理员/成员"
const char* role_name(int role);
/// UTF-8 字符串的显示宽度(中文按 2 列), 用于表格对齐
int display_width(const std::string& s);
/// 左对齐 + 右侧空格补齐到 width(按显示宽度, 而非字节数)
std::string pad(const std::string& s, int width);

// ------------------------------------------------------------
//  昵称 / 群名缓存
//  (协议无按 id 查资料 RPC, 只能由列表响应被动填充;
//   好友/群成员全覆盖, 未知 id 显示数字)
// ------------------------------------------------------------
class NickCache {
public:
    void update(uint32_t uid, const std::string& name);
    bool has(uint32_t uid) const;
    /// 已知返回昵称, 未知返回 "uid" 数字串
    std::string display(uint32_t uid) const;
private:
    mutable std::mutex m_mtx_;                  // reader 线程读, 主线程写
    std::unordered_map<uint32_t, std::string> map_;
};

class GroupCache {
public:
    void update(uint32_t gid, const std::string& name);
    bool has(uint32_t gid) const;
    std::string display(uint32_t gid) const;
private:
    mutable std::mutex m_mtx_;
    std::unordered_map<uint32_t, std::string> map_;
};

// ------------------------------------------------------------
//  会话状态(reader 线程经全局指针访问昵称/群名缓存)
// ------------------------------------------------------------
class Session {
public:
    int      fd = -1;
    uint32_t uid = 0;                 // 0 = 未登录
    std::string username, nickname;
    uint32_t target_id = 0;           // 会话目标(0 = 无)
    bool     target_group = false;
    NickCache  nick;
    GroupCache groups;

    bool logged_in() const { return uid != 0; }
    bool has_target() const { return target_id != 0; }

    /// 拼 readline prompt(带色码, 不可见序列以 \001\002 包裹)
    std::string prompt() const;
    /// 登录/登出/换目标后刷新 prompt(持锁 + rl_set_prompt + rl_redisplay)
    void refresh_prompt();

    /// 分发一条命令; 返回 false 表示退出。
    /// 首词命中命令集 → 命令; 否则有会话目标 → 直接发送; 否则未知命令。
    bool handle(const std::string& line);
};

// 当前会话(reader/心跳线程打印推送时取昵称)。在 main 里 start_reader 之前赋值。
extern Session* g_session;

// ------------------------------------------------------------
//  readline 胶水
// ------------------------------------------------------------
/// Tab 补全: 首词补全命令名, "friend" 后补全子命令
char** complete(const char* text, int start, int end);

}  // namespace ui
