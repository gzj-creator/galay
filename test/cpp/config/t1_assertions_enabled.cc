#include <cassert>

int main()
{
    int assertions_executed = 0;
    assert(++assertions_executed == 1);
    return assertions_executed == 1 ? 0 : 1;
}
