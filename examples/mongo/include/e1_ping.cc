#include <iostream>

#include "common/config.h"
#include "galay-mongo/sync/mongo_client.h"

using namespace galay::mongo;

int main()
{
    const auto cfg = mongo_example::loadMongoConfigFromEnv();

    MongoClient session;
    auto connected = session.connect(cfg);
    if (!connected) {
        std::cerr << "Connect failed: " << connected.error().message() << std::endl;
        return 1;
    }

    auto ping = session.ping(cfg.database);
    if (!ping) {
        std::cerr << "Ping failed: " << ping.error().message() << std::endl;
        session.close();
        return 1;
    }

    std::cout << "Mongo ping OK" << std::endl;
    session.close();
    return 0;
}
