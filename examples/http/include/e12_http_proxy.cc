#include "common/example_common.h"
#include "galay-http/server/http_router.h"
#include "galay-http/server/http_server.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <thread>
#include <chrono>

using namespace galay::http;
using namespace galay::kernel;

static std::string normalizeMountPrefix(std::string prefix) {
    if (prefix.empty()) {
        return "/static";
    }

    if (prefix.front() != '/') {
        prefix.insert(prefix.begin(), '/');
    }

    if (prefix.size() > 1 && prefix.back() == '/') {
        prefix.pop_back();
    }

    return prefix;
}

int main(int argc, char* argv[]) {
    uint16_t listen_port = example::kDefaultProxyPort;
    std::string upstream_host = "127.0.0.1";
    uint16_t upstream_port = example::kDefaultProxyUpstreamPort;
    std::string mount_prefix = "/static";
    std::string mount_dir = "./html";
    bool use_mount_hardly = false;
    bool use_nginx_try_files = false;
    bool use_raw_proxy = false;

    if (argc > 1) listen_port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc > 2) upstream_host = argv[2];
    if (argc > 3) upstream_port = static_cast<uint16_t>(std::atoi(argv[3]));
    if (argc > 4) mount_prefix = argv[4];
    if (argc > 5) mount_dir = argv[5];
    if (argc > 6) {
        std::string mode = argv[6];
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        use_mount_hardly = (mode == "hard" || mode == "hardly" || mode == "static");
        use_nginx_try_files = (mode == "nginx" || mode == "try" || mode == "try_files");
        use_raw_proxy = (mode == "raw" || mode == "nginx-raw" || mode == "try-raw");
        if (mode == "nginx-raw" || mode == "try-raw") {
            use_nginx_try_files = true;
        }
    }

    bool mount_enabled = !(mount_prefix == "off" || mount_prefix == "none" ||
                           mount_dir == "off" || mount_dir == "none");
    if (mount_enabled) {
        mount_prefix = normalizeMountPrefix(mount_prefix);
    }

    HttpRouter router;
    if (mount_enabled) {
        if (use_nginx_try_files) {
            router.tryFiles(mount_prefix, mount_dir, upstream_host, upstream_port,
                            StaticFileSetting(),
                            use_raw_proxy ? ProxyMode::Raw : ProxyMode::Http);
        } else if (use_mount_hardly) {
            router.mountHardly(mount_prefix, mount_dir);
        } else {
            router.mount(mount_prefix, mount_dir);
        }
    }

    router.proxy("/", upstream_host, upstream_port,
                 use_raw_proxy ? ProxyMode::Raw : ProxyMode::Http);

    HttpServer server(HttpServerBuilder()
        .host("0.0.0.0")
        .port(listen_port)
        .ioSchedulerCount(2)
        .build());
    std::cout << "Proxy listen : http://127.0.0.1:" << listen_port << "\n";
    std::cout << "Proxy target : http://" << upstream_host << ":" << upstream_port << "\n";
    std::cout << "Proxy mode   : " << (use_raw_proxy ? "raw" : "http") << "\n";
    if (mount_enabled) {
        std::cout << "Static mount : " << mount_prefix << " -> " << mount_dir
                  << " (" << (use_nginx_try_files ? "tryFiles(nginx)" :
                               (use_mount_hardly ? "mountHardly" : "mount")) << ")\n";
    } else {
        std::cout << "Static mount : disabled\n";
    }
    server.start(std::move(router));

    while (server.isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
