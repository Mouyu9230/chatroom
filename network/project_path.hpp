#pragma once

#include <climits>
#include <cstring>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

// ============================================================
//  项目根目录解析工具(client / server 共用, 头文件 inline)
//
//  背景: 文件传输的存放目录(server/files/ 与 client/files/)原先按
//  "相对当前工作目录"写死, 从任意目录启动时 files 会落到启动目录下,
//  而非仓库的 client/ 与 server/ 下 —— 用户从非仓库根启动就找不到。
//
//  project_root(): 从可执行文件路径(/proc/self/exe)向上逐级寻找
//  同时包含 client/ 与 server/ 源码目录的目录(即仓库根)。找不到则
//  回退到当前工作目录。返回以 '/' 结尾与否均可, 调用方自行拼接。
// ============================================================
inline std::string project_root() {
    // 可执行文件所在目录
    char buf[PATH_MAX];
    std::string exe_dir;
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        char* slash = std::strrchr(buf, '/');
        if (slash != nullptr) {
            *slash = '\0';
            exe_dir = buf;
        }
    }

    // 从 exe 目录逐级向上, 找同时含 client/ 与 server/ 源码目录的目录(仓库根)。
    // 必须校验为目录: build/ 下也有名为 client/server 的二进制文件, 只查存在会误判。
    std::string cur = exe_dir;
    while (!cur.empty() && cur != "/") {
        struct stat st_c, st_s;
        if (::stat((cur + "/client").c_str(), &st_c) == 0 && S_ISDIR(st_c.st_mode) &&
            ::stat((cur + "/server").c_str(), &st_s) == 0 && S_ISDIR(st_s.st_mode)) {
            return cur;
        }
        size_t pos = cur.find_last_of('/');
        if (pos == std::string::npos) break;
        cur = cur.substr(0, pos);
    }

    // 兜底: 找不到就退回当前工作目录
    return std::string(".");
}
