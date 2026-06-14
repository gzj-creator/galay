#include <iostream>
#include "mysql/sync/mysql_client.h"
#include "config.h"

using namespace galay::mysql;

int main()
{
    std::cout << "=== T4: Sync MySQL Client Tests ===" << std::endl;
    const auto db_cfg = mysql_test::loadMysqlTestConfig();
    if (const int skip_code = mysql_test::requireMysqlTestConfigOrSkip(db_cfg, "T4-SyncMysqlClient");
        skip_code != 0) {
        return skip_code;
    }
    mysql_test::printMysqlTestConfig(db_cfg);

    MysqlClient session;

    // 连接
    std::cout << "Connecting to MySQL server..." << std::endl;
    auto connect_result = session.connect(db_cfg.host, db_cfg.port, db_cfg.user, db_cfg.password, db_cfg.database);
    if (!connect_result) {
        std::cerr << "Connect failed: " << connect_result.error().message() << std::endl;
        return 1;
    }
    std::cout << "Connected successfully!" << std::endl;

    // 创建测试表
    auto create_result = session.query(
        "CREATE TABLE IF NOT EXISTS galay_sync_test ("
        "  id INT AUTO_INCREMENT PRIMARY KEY,"
        "  name VARCHAR(100),"
        "  value INT"
        ") ENGINE=InnoDB"
    );
    if (!create_result) {
        std::cerr << "CREATE TABLE failed: " << create_result.error().message() << std::endl;
        session.close();
        return 1;
    }
    std::cout << "Table created." << std::endl;

    // INSERT
    std::cout << "Testing INSERT..." << std::endl;
    auto insert_result = session.query("INSERT INTO galay_sync_test (name, value) VALUES ('sync_test', 42)");
    if (!insert_result) {
        std::cerr << "INSERT failed: " << insert_result.error().message() << std::endl;
    } else {
        std::cout << "  Affected rows: " << insert_result->affectedRows()
                  << ", Last insert ID: " << insert_result->lastInsertId() << std::endl;
    }

    // SELECT
    std::cout << "Testing SELECT..." << std::endl;
    auto select_result = session.query("SELECT * FROM galay_sync_test");
    if (!select_result) {
        std::cerr << "SELECT failed: " << select_result.error().message() << std::endl;
    } else {
        auto& rs = select_result.value();
        std::cout << "  Columns: " << rs.fieldCount() << ", Rows: " << rs.rowCount() << std::endl;
        for (size_t i = 0; i < rs.rowCount(); ++i) {
            auto& row = rs.row(i);
            std::cout << "  Row[" << i << "]:";
            for (size_t j = 0; j < row.size(); ++j) {
                std::cout << " " << row.getString(j, "NULL");
            }
            std::cout << std::endl;
        }
    }

    std::cout << "Testing PIPELINE batch..." << std::endl;
    protocol::MysqlCommandBuilder pipeline_builder;
    pipeline_builder.reserve(3, 3 * (protocol::MYSQL_PACKET_HEADER_SIZE + 1 + 16));
    pipeline_builder.appendQuery("SELECT 1");
    pipeline_builder.appendQuery("SELECT 2");
    pipeline_builder.appendQuery("SELECT 3");
    auto pipeline_result = session.batch(pipeline_builder.commands());
    if (!pipeline_result) {
        std::cerr << "PIPELINE batch failed: " << pipeline_result.error().message() << std::endl;
    } else {
        std::cout << "  Pipeline responses: " << pipeline_result->size() << std::endl;
    }

    // 清理
    session.query("DROP TABLE IF EXISTS galay_sync_test");
    session.close();

    std::cout << "\nAll sync tests completed." << std::endl;
    return 0;
}
