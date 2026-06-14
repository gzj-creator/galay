#include <iostream>
#include <string_view>

#include "config.h"

int main()
{
    const mysql_test::MysqlTestConfig defaults{};

    const auto require_empty = [](std::string_view field, const std::string& value) -> bool {
        if (!value.empty()) {
            std::cerr << "Expected " << field << " default to be empty, got `" << value << "`" << std::endl;
            return false;
        }
        return true;
    };

    if (!require_empty("host", defaults.host)) {
        return 1;
    }
    if (!require_empty("user", defaults.user)) {
        return 1;
    }
    if (!require_empty("password", defaults.password)) {
        return 1;
    }
    if (!require_empty("database", defaults.database)) {
        return 1;
    }
    if (defaults.port != 3306) {
        std::cerr << "Expected port default to remain 3306, got " << defaults.port << std::endl;
        return 1;
    }

    std::cout << "Mysql test defaults require environment variables." << std::endl;
    return 0;
}
