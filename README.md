# Galay

[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/CMake-3.21%2B-blue.svg)](https://cmake.org/)

Galay is a high-performance, cross-platform C++23 networking library that provides asynchronous I/O operations with coroutine support. It features a modern C++ design with support for TCP, UDP, and SSL/TLS protocols, along with advanced I/O mechanisms including epoll and kqueue.

## Features

### 🚀 **High Performance**
- **Modern I/O Mechanisms**: Automatic detection and use of epoll and kqueue
- **Coroutine Support**: Built-in C++23 coroutine support for asynchronous programming
- **Zero-Copy Operations**: Efficient memory management with minimal data copying
- **Multi-threaded Runtime**: Thread pool-based execution for optimal performance

### 🌐 **Network Protocols**
- **TCP Server/Client**: Full-featured TCP networking with async operations
- **UDP Server/Client**: High-performance UDP communication
- **SSL/TLS Support**: Secure connections with OpenSSL integration
- **Cross-platform**: Linux and macOS support

### 🛠 **Rich Utilities**
- **Cryptographic Algorithms**: MD5, SHA256, SHA512, Base64 encoding/decoding
- **Logging System**: Integrated spdlog-based logging with configurable levels
- **Command Line Parser**: Modern argument parsing with type safety
- **Error Handling**: Comprehensive error management system
- **Signal Handling**: Graceful signal processing with stack traces

### 📦 **Modern C++ Design**
- **C++23 Standard**: Latest C++ features including concepts and coroutines
- **Header-Only Components**: Many utilities available as header-only libraries
- **Template Metaprogramming**: Advanced type-safe programming patterns
- **RAII**: Resource management with automatic cleanup

## Quick Start

### Prerequisites

- **C++23 Compatible Compiler**: GCC 10+, Clang 12+, or MSVC 2019+
- **CMake 3.21+**
- **OpenSSL**: For SSL/TLS support
- **spdlog**: For logging functionality
- **concurrentqueue**: For lock-free data structures
- **libcuckoo**: For high-performance hash tables
- **libaio**: For file I/O (Linux dependency)

### Installation

#### Linux/macOS

```bash
# Clone the repository
git clone https://github.com/your-username/galay.git
cd galay

# Create build directory
mkdir build && cd build

# Configure with CMake
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build the library
make -j$(nproc)

# Install (optional)
sudo make install
```

#### Windows

```cmd
# Clone the repository
git clone https://github.com/your-username/galay.git
cd galay

# Create build directory
mkdir build && cd build

# Configure with CMake
cmake .. -G "Visual Studio 16 2019" -A x64

# Build the library
cmake --build . --config Release
```

### Basic Usage

#### TCP Server Example

```cpp
#include "galay/kernel/server/TcpServer.h"

using namespace galay;

int main() {
    TcpServerBuilder builder;
    builder.addListen({"0.0.0.0", 8080});
    
    TcpServer server = builder
        .startCoChecker(true, std::chrono::milliseconds(1000))
        .build();
    
    server.run([](AsyncTcpSocket& socket) -> Coroutine<nil> {
        while(true) {
            auto rwrapper = co_await socket.recv(1024);
            if(!rwrapper.success()) {
                if(rwrapper.getError()->code() == error::ErrorCode::DisConnectError) {
                    co_await socket.close();
                    co_return nil();
                }
                co_return nil();
            }
            
            Bytes bytes = rwrapper.moveValue();
            std::string msg = bytes.toString();
            std::cout << "Received: " << msg << std::endl;
            
            // Echo back the message
            auto wwrapper = co_await socket.send(std::move(bytes));
            if(!wwrapper.success()) {
                std::cout << "Send error: " << wwrapper.getError()->message() << std::endl;
            }
        }
    });
    
    server.wait();
    return 0;
}
```

#### SSL/TLS Server Example

```cpp
#include "galay/kernel/server/TcpSslServer.h"

using namespace galay;

int main() {
    TcpSslServerBuilder builder;
    builder.sslConf("server.crt", "server.key");
    builder.addListen({"0.0.0.0", 8443});
    
    TcpSslServer server = builder
        .startCoChecker(true, std::chrono::milliseconds(1000))
        .build();
    
    server.run([](AsyncSslSocket& socket) -> Coroutine<nil> {
        while(true) {
            auto rwrapper = co_await socket.sslRecv(1024);
            if(!rwrapper.success()) {
                if(rwrapper.getError()->code() == error::ErrorCode::DisConnectError) {
                    co_await socket.sslClose();
                    co_return nil();
                }
                co_return nil();
            }
            
            Bytes bytes = rwrapper.moveValue();
            auto wwrapper = co_await socket.sslSend(std::move(bytes));
        }
    });
    
    server.wait();
    return 0;
}
```

#### UDP Server Example

```cpp
#include "galay/kernel/server/UdpServer.h"

using namespace galay;

int main() {
    UdpServerBuilder builder;
    builder.addListen({"0.0.0.0", 8080});
    
    UdpServer server = builder
        .startCoChecker(true, std::chrono::milliseconds(1000))
        .build();
    
    server.run([](AsyncUdpSocket& socket) -> Coroutine<nil> {
        while(true) {
            Host remote;
            auto rwrapper = co_await socket.recvfrom(remote, 1024);
            if(!rwrapper.success()) {
                co_return nil();
            }
            
            Bytes bytes = rwrapper.moveValue();
            std::cout << "Received from " << remote.ip << ":" << remote.port << std::endl;
            
            // Echo back to sender
            auto wwrapper = co_await socket.sendto(remote, std::move(bytes));
        }
    });
    
    server.wait();
    return 0;
}
```

## Architecture

### Core Modules

#### 🧮 **Algorithm Module**
- **Cryptographic Functions**: MD5, SHA256, SHA512 hashing
- **Encoding/Decoding**: Base64 with multiple variants (PEM, MIME, URL-safe)
- **Hash Functions**: MurmurHash3 for fast hashing
- **Salt Generation**: Secure random salt generation

#### 🔧 **Common Module**
- **Base Types**: Cross-platform handle definitions and event types
- **Error Handling**: Comprehensive error management system
- **Logging**: Integrated logging with spdlog
- **Reflection**: Runtime type information and serialization
- **Strategy Pattern**: Pluggable algorithm implementations

#### ⚡ **Kernel Module**
- **Async I/O**: File, network, and timer event handling
- **Coroutines**: C++23 coroutine support for async operations
- **Event Loop**: High-performance event-driven architecture
- **Runtime**: Thread pool and scheduler management
- **Servers**: TCP, UDP, and SSL server implementations
- **Clients**: TCP, UDP, and SSL client implementations

#### 🛠 **Utils Module**
- **Application Framework**: Command-line argument parsing
- **Backtrace**: Stack trace generation for debugging
- **Circuit Breaker**: Fault tolerance patterns
- **Distributed Systems**: Distributed computing utilities
- **Data Structures**: Pools, trees, and specialized containers
- **Rate Limiting**: Traffic control and throttling
- **String Utilities**: Advanced string manipulation
- **System Information**: Platform-specific system calls

## Build Configuration

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_DEBUG` | ON | Enable debug symbols and assertions |
| `ENABLE_INSTALL_SYSTEM` | ON | Install to system directories |
| `BUILD_STATIC` | OFF | Build static library instead of shared |
| `ENABLE_SYSTEM_LOG` | ON | Enable system logging |
| `ENABLE_LOG_TRACE` | ON | Enable trace-level logging |
| `ENABLE_DEFAULT_USE_EPOLL` | ON | Force epoll usage on Linux |
| `ENABLE_GTEST` | AUTO | Enable Google Test integration |
| `ENABLE_NLOHMANN_JSON` | AUTO | Enable JSON support |

### Platform-Specific I/O

The library automatically detects and uses the best available I/O mechanism:

- **Linux**: epoll
- **macOS/FreeBSD**: kqueue

## Testing

The project includes comprehensive test suites for all major components:

```bash
# Build tests
cd build
make

# Run all tests
cd test
./test_tcp_server    # TCP server functionality
./test_ssl_server    # SSL/TLS server functionality
./test_udp_server    # UDP server functionality
./test_tcp_client    # TCP client functionality
./test_ssl_client    # SSL/TLS client functionality
./test_udp_client    # UDP client functionality
./test_file          # File I/O operations
./test_time          # Timer functionality
```

## Performance

Galay is designed for high-performance networking applications:

- **Zero-copy I/O**: Minimizes memory copying operations
- **Lock-free Data Structures**: Uses concurrentqueue and libcuckoo
- **Modern I/O**: Efficient I/O operations with epoll and kqueue
- **Coroutine Efficiency**: Stackless coroutines for minimal overhead
- **Thread Pool**: Optimal CPU utilization with work-stealing

### Development Setup

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install libssl-dev libspdlog-dev libgtest-dev

# Install concurrentqueue
git clone https://github.com/cameron314/concurrentqueue.git
sudo cp -r concurrentqueue/concurrentqueue /usr/local/include/

# Install libcuckoo
git clone https://github.com/efficient/libcuckoo.git
sudo cp -r libcuckoo/libcuckoo /usr/local/include/

# Build with debug symbols
mkdir build && cd build
cmake .. -DENABLE_DEBUG=ON
make -j$(nproc)
```

## License

This project is licensed under the Apache License 2.0 - see the [LICENSE](LICENSE) file for details.


## Roadmap

- [ ] IOCP support
- [ ] io_uring support

---

**Galay** - High-performance C++23 networking library with coroutine support.
