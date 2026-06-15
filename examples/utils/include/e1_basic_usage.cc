#include <galay-utils/galay_utils.hpp>

#include <iostream>

int main() {
    using namespace galay::utils;

    auto parts = StringUtils::split("hello,galay,utils", ',');
    std::cout << StringUtils::join(parts, " ") << std::endl;
    std::cout << "cpu cores: " << System::cpuCount() << std::endl;
    return 0;
}
