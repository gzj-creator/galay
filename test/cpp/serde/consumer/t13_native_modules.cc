#include <cassert>
#include <string>
#include <type_traits>

import galay.kernel;
import galay.postgres;
import galay.rpc;
import json;
import toml;

static_assert(!std::is_move_constructible_v<galay::kernel::IOController>);

int main()
{
    const auto config = galay::postgres::PostgresConfig::default_config();
    assert(!config.host.empty());

    galay::rpc::RpcRequest request;
    request.request_id(42);
    assert(request.request_id() == 42);
    return 0;
}
