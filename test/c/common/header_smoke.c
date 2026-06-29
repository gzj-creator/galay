#include <galay/c/galay-common-c/common/galay_c_defs.h>
#include <galay/c/galay-common-c/common/galay_c_error.h>

int main(void)
{
    return galay_status_string(GALAY_OK) != 0 ? 0 : 1;
}
