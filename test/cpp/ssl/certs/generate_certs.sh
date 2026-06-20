#!/bin/bash
# 生成测试用的自签名证书

CERT_DIR="$(dirname "$0")"
cd "$CERT_DIR"

# 生成 CA 私钥
openssl genrsa -out ca.key 2048

# 生成 CA 证书
openssl req -new -x509 -days 365 -key ca.key -out ca.crt \
    -subj "/C=CN/ST=Test/L=Test/O=Test CA/CN=Test CA"

# 生成服务器私钥
openssl genrsa -out server.key 2048

# 生成服务器证书签名请求
openssl req -new -key server.key -out server.csr \
    -subj "/C=CN/ST=Test/L=Test/O=Test Server/CN=localhost"

# 使用 CA 签名服务器证书
openssl x509 -req -days 365 -in server.csr -CA ca.crt -CAkey ca.key \
    -CAcreateserial -out server.crt

# 生成客户端私钥
openssl genrsa -out client.key 2048

# 生成客户端证书签名请求
openssl req -new -key client.key -out client.csr \
    -subj "/C=CN/ST=Test/L=Test/O=Test Client/CN=Test Client"

# 使用 CA 签名客户端证书
openssl x509 -req -days 365 -in client.csr -CA ca.crt -CAkey ca.key \
    -CAcreateserial -out client.crt

# 清理 CSR 文件
rm -f *.csr

echo "Certificates generated successfully!"
echo "Files:"
ls -la *.crt *.key
