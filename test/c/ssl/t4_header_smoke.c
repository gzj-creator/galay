#include <galay/c/galay-ssl-c/ssl.h>

int main(void)
{
    return GALAY_SSL_VERIFY_NONE == 0 ? 0 : 1;
}
