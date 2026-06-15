#include <iostream>
#include <optional>
#include <vector>
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

    auto begin = session.beginTransaction();
    if (!begin) {
        std::cerr << "begin transaction failed: " << begin.error().message() << std::endl;
        session.close();
        return 1;
    }

    auto prep = session.prepare("SELECT ? + ?");
    if (!prep) {
        std::cerr << "prepare failed: " << prep.error().message() << std::endl;
        session.rollback();
        session.close();
        return 1;
    }

    std::vector<std::optional<std::string>> params = {"3", "5"};
    auto exec = session.stmtExecute(prep->statement_id, params);
    if (!exec) {
        std::cerr << "stmtExecute failed: " << exec.error().message() << std::endl;
        session.stmtClose(prep->statement_id);
        session.rollback();
        session.close();
        return 1;
    }

    std::cout << "[E4] prepared SELECT returned " << exec->rowCount() << " row(s)" << std::endl;

    session.stmtClose(prep->statement_id);
    session.commit();
    session.close();
    return 0;
}
