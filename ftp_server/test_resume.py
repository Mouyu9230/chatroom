#!/usr/bin/env python3
"""断点续传功能测试：模拟传输中断后 REST 续传。

运行前需先启动服务器： ./build/server
"""
import os
import socket
import sys
import time

HOST = "127.0.0.1"
PORT = 2100
CTRL_BUF = 8192


def read_resp(ctrl):
    """读取一行 FTP 控制响应。"""
    data = b""
    while not data.endswith(b"\r\n"):
        chunk = ctrl.recv(1)
        if not chunk:
            break
        data += chunk
    return data.decode().strip()


def pasv(ctrl):
    """发送 PASV 并返回新数据连接。"""
    ctrl.sendall(b"PASV\r\n")
    resp = read_resp(ctrl)
    print("  <", resp)
    if not resp.startswith("227"):
        raise RuntimeError("PASV failed: " + resp)
    inside = resp[resp.find("(") + 1: resp.find(")")]
    nums = [int(x) for x in inside.split(",")]
    dport = nums[4] * 256 + nums[5]
    d = socket.create_connection((HOST, dport), timeout=5)
    return d


def login(ctrl):
    ctrl.sendall(b"USER 1\r\n"); print("  <", read_resp(ctrl))
    ctrl.sendall(b"PASS 1\r\n"); print("  <", read_resp(ctrl))


def recv_exact(sock, n):
    """从数据连接精确读取 n 字节。"""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return buf


def test_retr_resume():
    print("\n===== 测试1: RETR 下载断点续传 =====")
    original = open("test_svr/resume_test.bin", "rb").read()
    n_interrupt = 4000

    ctrl = socket.create_connection((HOST, PORT), timeout=5)
    read_resp(ctrl)  # 220

    login(ctrl)

    # 第一次下载：读 4000 字节后"中断"（关闭数据连接）
    d = pasv(ctrl)
    ctrl.sendall(b"RETR resume_test.bin\r\n")
    resp = read_resp(ctrl)
    print("  <", resp)
    assert resp.startswith("150")
    part1 = recv_exact(d, n_interrupt)
    print(f"  [中断] 已下载 {len(part1)} 字节，关闭数据连接模拟断网")
    d.close()
    read_resp(ctrl)  # 226 (服务端收尾)

    # 第二次：REST 4000 续传
    d = pasv(ctrl)
    ctrl.sendall(b"REST 4000\r\n")
    resp = read_resp(ctrl)
    print("  <", resp)
    assert resp.startswith("350"), "REST 失败: " + resp
    ctrl.sendall(b"RETR resume_test.bin\r\n")
    resp = read_resp(ctrl)
    print("  <", resp)
    assert resp.startswith("150")
    part2 = b""
    while True:
        chunk = d.recv(CTRL_BUF)
        if not chunk:
            break
        part2 += chunk
    d.close()
    print("  <", read_resp(ctrl))  # 226

    merged = part1 + part2
    print(f"  第一次下载 {len(part1)} + 续传下载 {len(part2)} = {len(merged)}，原文件 {len(original)}")
    assert merged == original, "RETR 续传结果与原文件不一致!"
    ctrl.close()
    print("  [PASS] RETR 断点续传正确")


def test_stor_resume():
    print("\n===== 测试2: STOR 上传断点续传 =====")
    local = open("test_cli/up.bin", "rb").read()
    n_interrupt = 3000

    ctrl = socket.create_connection((HOST, PORT), timeout=5)
    read_resp(ctrl)
    login(ctrl)

    # 第一次上传：只发送 3000 字节后"中断"
    d = pasv(ctrl)
    ctrl.sendall(b"STOR up.bin\r\n")
    resp = read_resp(ctrl)
    print("  <", resp)
    assert resp.startswith("150")
    d.sendall(local[:n_interrupt])
    print(f"  [中断] 已上传 {n_interrupt} 字节，关闭数据连接模拟断网")
    d.close()
    read_resp(ctrl)  # 226

    # 查询服务器已存大小
    ctrl.sendall(b"SIZE up.bin\r\n")
    resp = read_resp(ctrl)
    print("  <", resp)
    assert resp.startswith("213"), "SIZE 失败: " + resp
    size = int(resp[4:])
    print(f"  [续传] 服务器已存 {size} 字节")

    # 续传剩余部分
    d = pasv(ctrl)
    ctrl.sendall(b"REST %d\r\n" % size)
    resp = read_resp(ctrl)
    print("  <", resp)
    assert resp.startswith("350")
    ctrl.sendall(b"STOR up.bin\r\n")
    resp = read_resp(ctrl)
    print("  <", resp)
    assert resp.startswith("150")
    d.sendall(local[size:])
    d.close()
    print("  <", read_resp(ctrl))  # 226

    ctrl.close()

    got = open("test_svr/svr_stor_up.bin", "rb").read()
    print(f"  服务器最终文件 {len(got)} 字节，本地原文件 {len(local)} 字节")
    assert got == local, "STOR 续传后服务器文件与本地不一致!"
    print("  [PASS] STOR 断点续传正确")


if __name__ == "__main__":
    # 生成测试文件
    with open("test_svr/resume_test.bin", "wb") as f:
        f.write(bytes(range(256)) * 40)  # 10240 字节
    with open("test_cli/up.bin", "wb") as f:
        f.write(bytes(range(256)) * 32)  # 8192 字节

    # 清理上次测试遗留
    for p in ("test_svr/svr_stor_up.bin",):
        if os.path.exists(p):
            os.remove(p)

    time.sleep(0.2)
    test_retr_resume()
    test_stor_resume()
    print("\n全部测试通过 ✅")
