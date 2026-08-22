#pragma once

#include <climits>
#include <cstring>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

// ============================================================
//  项目根目录解析工具(client / server 共用)。
//  project_root(): 从 /proc/self/exe 向上逐级找同时含 client/ 与 server/ 目录
//  的仓库根(避免 files 目录从任意 cwd 启动时落到启动目录); 找不到回退 cwd。
//  调用方自行拼接路径。
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
