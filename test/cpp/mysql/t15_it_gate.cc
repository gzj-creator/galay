#include "test/cpp/mysql/config.h"

#include <iostream>

int main()
{
    if (const int skip_code = mysql_test::requireIntegrationEnabledOrSkip("T15-IntegrationGate");
        skip_code != 0) {
        return skip_code;
    }

    if (!mysql_test::isIntegrationEnabled()) {
        std::cerr << "T15-IntegrationGate failed: integration gate should be enabled." << std::endl;
        return 1;
    }

    std::cout << "T15-IntegrationGate enabled." << std::endl;
    return 0;
}
