#include <iostream>
#include "common/config.h"
#include "galay-mysql/sync/mysql_client.h"

using namespace galay::mysql;

int main()
{
    const auto cfg = mysql_example::loadMysqlExampleConfig();
    mysql_example::printMysqlExampleConfig(cfg);

    MysqlClient session;
    auto conn = session.connect(cfg.host, cfg.port, cfg.user, cfg.password, cfg.database);
    if (!conn) {
        std::cerr << "connect failed: " << conn.error().message() << std::endl;
        return 1;
    }

    auto res = session.query("SELECT NOW()");
    if (!res) {
        std::cerr << "query failed: " << res.error().message() << std::endl;
        session.close();
        return 1;
    }

    if (res->rowCount() > 0) {
        std::cout << "[E2] NOW() => " << res->row(0).getString(0) << std::endl;
    }

    session.close();
    return 0;
}
