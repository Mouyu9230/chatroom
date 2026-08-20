#!/usr/bin/env bash
# 生成开发/内网用自签名证书 (server.crt / server.key)
#
# 用法: ./gen_cert.sh
# 之后在 server 同目录运行 server 即可(默认加载 server.crt / server.key)。
#
# 生产环境请使用 CA 签发的证书, 不要用本脚本:
#   1) 申请 Let's Encrypt 等公共证书, 或内网自建 CA;
#   2) 客户端改为 SSL_VERIFY_PEER + 加载 CA 校验。
set -euo pipefail
cd "$(dirname "$0")"

openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout server.key -out server.crt -days 365 \
  -subj "/CN=127.0.0.1" \
  -addext "subjectAltName=IP:127.0.0.1,DNS:localhost"

chmod 600 server.key
echo "generated:"
echo "  $(pwd)/server.crt"
echo "  $(pwd)/server.key"
echo "(self-signed, 365 days; server 会自动在 当前目录 / build 上级 等位置查找)"
