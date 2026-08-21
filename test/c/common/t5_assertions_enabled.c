#include <assert.h>

int main(void)
{
    int assertions_executed = 0;
    assert(++assertions_executed == 1);
    return assertions_executed == 1 ? 0 : 1;
}
