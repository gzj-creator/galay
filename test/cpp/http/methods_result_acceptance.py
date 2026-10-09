import http.server
import subprocess
import sys
import threading


class MethodsHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    reject_post = False
    disconnect_get = False

    def handle_method(self):
        size = int(self.headers.get("Content-Length", "0"))
        if size:
            self.rfile.read(size)
        if self.disconnect_get and self.command == "GET":
            self.close_connection = True
            return
        code = 500 if self.reject_post and self.command == "POST" else 200
        body = b"methods fixture"
        self.send_response(code)
        self.send_header("Content-Length", str(42 if self.command == "HEAD" else
                                               0 if self.command == "CONNECT" else len(body)))
        self.end_headers()
        if self.command not in ("HEAD", "CONNECT"):
            self.wfile.write(body)

    do_GET = handle_method
    do_POST = handle_method
    do_PUT = handle_method
    do_DELETE = handle_method
    do_HEAD = handle_method
    do_OPTIONS = handle_method
    do_PATCH = handle_method
    do_TRACE = handle_method
    do_CONNECT = handle_method

    def log_message(self, *args):
        pass


def run_client(binary, expected_passed):
    result = subprocess.run([binary], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=25)
    print(result.stdout)
    expected_code = 0 if expected_passed == 9 else 1
    if result.returncode != expected_code:
        raise AssertionError(f"expected exit={expected_code}, exit={result.returncode}")
    summary = f"HTTP methods passed={expected_passed} failed={9 - expected_passed}"
    if summary not in result.stdout:
        raise AssertionError(f"missing completed result: {summary}")


def main(binary):
    with http.server.ThreadingHTTPServer(("127.0.0.1", 8080), MethodsHandler) as server:
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            MethodsHandler.reject_post = True
            run_client(binary, 8)
            MethodsHandler.reject_post = False
            run_client(binary, 9)
            MethodsHandler.disconnect_get = True
            run_client(binary, 8)
        finally:
            server.shutdown()
            thread.join()
    run_client(binary, 0)


if __name__ == "__main__":
    main(sys.argv[1])
