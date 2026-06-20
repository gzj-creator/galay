# Test Certificates

This directory contains test certificates for SSL/TLS testing.

## Generate Certificates

Run the following command to generate test certificates:

```bash
chmod +x generate_certs.sh
./generate_certs.sh
```

## Files

After running the script, you will have:

- `ca.key` - CA private key
- `ca.crt` - CA certificate
- `server.key` - Server private key
- `server.crt` - Server certificate (signed by CA)
- `client.key` - Client private key
- `client.crt` - Client certificate (signed by CA)

## Usage

### Server
```cpp
SslContext ctx(SslMethod::TLS_Server);
ctx.loadCertificate("server.crt");
ctx.loadPrivateKey("server.key");
```

### Client (with verification)
```cpp
SslContext ctx(SslMethod::TLS_Client);
ctx.setVerifyMode(SslVerifyMode::Peer);
ctx.loadCACertificate("ca.crt");
```

### Client (without verification, for testing only)
```cpp
SslContext ctx(SslMethod::TLS_Client);
ctx.setVerifyMode(SslVerifyMode::None);
```

## Warning

These certificates are for testing purposes only. Do NOT use them in production!
