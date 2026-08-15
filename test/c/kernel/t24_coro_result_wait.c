#include <galay/c/galay-kernel-c/coro-c/coro_wait.h>

int main(void)
{
    galay_c_io_controller_t controller = {0};
    char buffer[8] = {0};

    if (galay_c_coro_wait_io(NULL, &controller, GALAY_C_EVENT_READ, 0).code !=
            C_IOResultInvalid ||
        galay_c_coro_wait_io(NULL, NULL, GALAY_C_EVENT_READ, 0).code !=
            C_IOResultInvalid ||
        galay_c_coro_cancel_io(NULL, &controller, GALAY_C_EVENT_READ).code !=
            C_IOResultInvalid ||
        galay_c_coro_recv_blocking(NULL, &controller, buffer, sizeof(buffer), 0).code !=
            C_IOResultInvalid ||
        galay_c_coro_send_blocking(NULL, &controller, buffer, sizeof(buffer), 0).code !=
            C_IOResultInvalid) {
        return 1;
    }
    return 0;
}
