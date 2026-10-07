import galay.mysql;

#include <iostream>
#include <optional>
#include <vector>
#include "common/config.h"

using namespace galay::mysql;

int main()
{
    const auto cfg = mysql_example::load_db_example_config();
    mysql_example::print_db_example_config(cfg);

    MysqlClient session;
    auto conn = session.connect(cfg.host, cfg.port, cfg.user, cfg.password, cfg.database);
    if (!conn) {
        std::cerr << "connect failed: " << conn.error().message() << std::endl;
        return 1;
    }

    auto begin = session.begin_transaction();
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
    auto exec = session.stmt_execute(prep->statement_id, params);
    if (!exec) {
        std::cerr << "stmtExecute failed: " << exec.error().message() << std::endl;
        session.stmt_close(prep->statement_id);
        session.rollback();
        session.close();
        return 1;
    }

    std::cout << "[E4-import] prepared SELECT returned " << exec->row_count() << " row(s)" << std::endl;

    session.stmt_close(prep->statement_id);
    session.commit();
    session.close();
    return 0;
}
