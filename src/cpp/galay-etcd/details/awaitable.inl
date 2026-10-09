/**
 * @file awaitable.inl
 * @brief etcd 异步客户端 Awaitable 实现
 */

details::PostJsonAwaitable::Context::Context(AsyncEtcdClient& client,
                                             std::string api_path,
                                             std::string body)
    : awaitable(client.m_http_session->send_serialized_request(
          client.build_serialized_post_request(api_path, body)))
    , owner(&client)
{
}

details::PostJsonAwaitable::PostJsonAwaitable(
    AsyncEtcdClient& client,
    std::string api_path,
    std::string body,
    std::optional<std::chrono::milliseconds> force_timeout)
    : m_ctx(std::nullopt)
{
    if (!client.m_connected || client.m_socket == nullptr || client.m_http_session == nullptr) {
        client.set_error(EtcdErrorType::NotConnected, "etcd client is not connected");
        return;
    }

    m_ctx.emplace(client, std::move(api_path), std::move(body));

    if (force_timeout.has_value()) {
        m_ctx->awaitable.timeout(force_timeout.value());
    } else if (client.m_network_config.is_request_timeout_enabled()) {
        m_ctx->awaitable.timeout(client.m_network_config.request_timeout);
    }
}

bool details::PostJsonAwaitable::await_ready() const noexcept
{
    return !m_ctx.has_value();
}

std::expected<std::string, EtcdError> details::PostJsonAwaitable::await_resume()
{
    if (!m_ctx.has_value()) {
        ETCD_LOG_WARN("[async] [request]", "request rejected error=etcd client is not connected");
        return std::unexpected(EtcdError(EtcdErrorType::NotConnected, "etcd client is not connected"));
    }

    auto response_result = m_ctx->awaitable.await_resume();
    if (!response_result.has_value()) {
        const auto mapped = map_http_error(response_result.error());
        m_ctx->owner->set_error(mapped);
        ETCD_LOG_ERROR("[async] [request]", "http request failed endpoint={} error={}",
                       m_ctx->owner->m_config.endpoint,
                       mapped.message());
        return std::unexpected(mapped);
    }

    if (!response_result->has_value()) {
        EtcdError error(EtcdErrorType::Internal, "http response incomplete");
        m_ctx->owner->set_error(error);
        ETCD_LOG_ERROR("[async] [request]", "http response incomplete endpoint={}",
                       m_ctx->owner->m_config.endpoint);
        return std::unexpected(error);
    }

    auto response = std::move(response_result->value());
    const int status_code = static_cast<int>(response.header().code());
    const std::string response_body = response.get_body_str();

    if (status_code < 200 || status_code >= 300) {
        EtcdError error(
            EtcdErrorType::Server,
            "HTTP status=" + std::to_string(status_code) +
            ", body=" + response_body);
        m_ctx->owner->set_error(error);
        ETCD_LOG_WARN("[async] [request]", "unexpected http status endpoint={} status={} body_size={}",
                      m_ctx->owner->m_config.endpoint,
                      status_code,
                      response_body.size());
        return std::unexpected(error);
    }

    ETCD_LOG_DEBUG("[async] [request]", "request completed endpoint={} status={} body_size={}",
                   m_ctx->owner->m_config.endpoint,
                   status_code,
                   response_body.size());

    return response_body;
}

details::JsonOpAwaitableBase::JsonOpAwaitableBase(AsyncEtcdClient& client)
    : m_client(&client)
{
}

void details::JsonOpAwaitableBase::start_post(
    std::string api_path,
    std::string body,
    std::optional<std::chrono::milliseconds> force_timeout)
{
    m_post_awaitable.emplace(*m_client, std::move(api_path), std::move(body), force_timeout);
}

bool details::JsonOpAwaitableBase::await_ready() const noexcept
{
    return !m_post_awaitable.has_value() || m_post_awaitable->await_ready();
}

std::expected<std::string, EtcdError> details::JsonOpAwaitableBase::resume_post()
{
    return m_client->resume_post_or_current(
        m_post_awaitable.has_value() ? &*m_post_awaitable : nullptr);
}

details::PutAwaitable::PutAwaitable(AsyncEtcdClient& client,
                                   std::string key,
                                   std::string value,
                                   std::optional<int64_t> lease_id)
    : JsonOpAwaitableBase(client)
{
    m_client->reset_last_operation();
    auto body = build_put_request_body(key, value, lease_id);
    if (!body.has_value()) {
        m_client->set_error(body.error());
        return;
    }

    start_post("/kv/put", std::move(body.value()));
}

bool details::PutAwaitable::await_ready() const noexcept
{
    return JsonOpAwaitableBase::await_ready();
}

EtcdBoolResult details::PutAwaitable::await_resume()
{
    auto response_body = resume_post();
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto put_result = parse_put_response(response_body.value());
    if (!put_result.has_value()) {
        m_client->set_error(put_result.error());
        return std::unexpected(put_result.error());
    }

    return true;
}

details::ConnectAwaitable::SharedState::SharedState(AsyncEtcdClient& owner)
    : client(&owner)
{
    client->reset_last_operation();
    if (client->m_scheduler == nullptr) {
        EtcdError error(EtcdErrorType::Internal, "IOScheduler is null");
        client->set_error(error);
        ETCD_LOG_ERROR("[async] [connect]", "scheduler is null endpoint={}",
                       client->m_config.endpoint);
        result = std::unexpected(error);
        return;
    }

    if (client->m_connected && client->m_socket != nullptr && client->m_http_session != nullptr) {
        ETCD_LOG_DEBUG("[async] [connect]", "already connected endpoint={}",
                       client->m_config.endpoint);
        result = true;
        return;
    }

    if (!client->m_endpoint_valid || !client->m_server_host.has_value()) {
        const std::string message = client->m_endpoint_error.empty()
            ? "invalid endpoint"
            : client->m_endpoint_error;
        EtcdError error(EtcdErrorType::InvalidEndpoint, message);
        client->set_error(error);
        ETCD_LOG_ERROR("[async] [connect]", "invalid endpoint endpoint={} error={}",
                       client->m_config.endpoint,
                       error.message());
        result = std::unexpected(error);
        return;
    }

    try {
        client->m_socket = std::make_unique<galay::async::AsyncTcpSocket>(client->m_ip_type);
        auto nonblock_result = client->m_socket->option().handle_non_block();
        if (!nonblock_result.has_value()) {
            EtcdError error = map_kernel_io_error(nonblock_result.error(), EtcdErrorType::Connection);
            client->set_error(error);
            ETCD_LOG_ERROR("[async] [connect]", "set nonblocking failed endpoint={} error={}",
                           client->m_config.endpoint,
                           error.message());
            client->m_socket.reset();
            client->m_connected = false;
            result = std::unexpected(error);
            return;
        }

        if (client->m_network_config.tcp_no_delay) {
            auto nodelay_result = client->m_socket->option().handle_tcp_no_delay();
            if (!nodelay_result.has_value()) {
                EtcdError error = map_kernel_io_error(nodelay_result.error(), EtcdErrorType::Connection);
                client->set_error(error);
                ETCD_LOG_ERROR("[async] [connect]", "set TCP_NODELAY failed endpoint={} error={}",
                               client->m_config.endpoint,
                               error.message());
                client->m_socket.reset();
                client->m_connected = false;
                result = std::unexpected(error);
                return;
            }
        }

        host = client->m_server_host.value();
        phase = Phase::Connect;
        ETCD_LOG_INFO("[async] [connect]", "connecting endpoint={}",
                      client->m_config.endpoint);
    } catch (const std::exception& ex) {
        EtcdError error(EtcdErrorType::Connection, ex.what());
        client->set_error(error);
        ETCD_LOG_ERROR("[async] [connect]", "prepare connect failed endpoint={} error={}",
                       client->m_config.endpoint,
                       error.message());
        client->m_http_session.reset();
        client->m_socket.reset();
        client->m_connected = false;
        result = std::unexpected(error);
    }
}

details::ConnectAwaitable::Machine::Machine(std::shared_ptr<SharedState> state)
    : m_state(std::move(state))
{
}

galay::kernel::MachineAction<details::ConnectAwaitable::Result>
details::ConnectAwaitable::Machine::advance()
{
    if (m_state->result.has_value()) {
        return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
    }

    if (m_state->phase == Phase::Connect) {
        return galay::kernel::MachineAction<result_type>::wait_connect(m_state->host);
    }

    m_state->result = m_state->client->current_bool_result();
    return galay::kernel::MachineAction<result_type>::complete(std::move(*m_state->result));
}

void details::ConnectAwaitable::Machine::on_connect(
    std::expected<void, galay::kernel::IOError> result)
{
    if (!result.has_value()) {
        EtcdError error = map_kernel_io_error(result.error());
        m_state->client->set_error(error);
        ETCD_LOG_ERROR("[async] [connect]", "connect failed endpoint={} error={}",
                       m_state->client->m_config.endpoint,
                       error.message());
        m_state->client->m_http_session.reset();
        m_state->client->m_socket.reset();
        m_state->client->m_connected = false;
        m_state->result = std::unexpected(error);
        m_state->phase = Phase::Done;
        return;
    }

    try {
        m_state->client->m_http_session = std::make_unique<galay::http::HttpSession>(
            *m_state->client->m_socket,
            m_state->client->m_network_config.buffer_size);
        m_state->client->m_connected = true;
        m_state->result = true;
        ETCD_LOG_INFO("[async] [connect]", "connected endpoint={}",
                      m_state->client->m_config.endpoint);
    } catch (const std::exception& ex) {
        EtcdError error(EtcdErrorType::Internal,
                        std::string("create http session failed: ") + ex.what());
        m_state->client->set_error(error);
        ETCD_LOG_ERROR("[async] [connect]", "create http session failed endpoint={} error={}",
                       m_state->client->m_config.endpoint,
                       error.message());
        m_state->client->m_http_session.reset();
        m_state->client->m_socket.reset();
        m_state->client->m_connected = false;
        m_state->result = std::unexpected(error);
    }

    m_state->phase = Phase::Done;
}

void details::ConnectAwaitable::Machine::on_read(
    std::expected<size_t, galay::kernel::IOError>)
{
}

void details::ConnectAwaitable::Machine::on_write(
    std::expected<size_t, galay::kernel::IOError>)
{
}

details::ConnectAwaitable::ConnectAwaitable(AsyncEtcdClient& client)
    : m_state(std::make_shared<SharedState>(client))
{
    auto* controller =
        client.m_socket != nullptr ? client.m_socket->controller() : &invalid_controller();
    m_inner = std::make_unique<InnerAwaitable>(
        galay::kernel::AwaitableBuilder<Result>::from_state_machine(
            controller,
            Machine(m_state))
            .build());
}

bool details::ConnectAwaitable::await_ready() noexcept
{
    return m_inner->await_ready();
}

EtcdBoolResult details::ConnectAwaitable::await_resume()
{
    return m_inner->await_resume();
}

details::CloseAwaitable::CloseAwaitable(AsyncEtcdClient& client)
    : IoAwaitableBase(client)
{
    m_client->reset_last_operation();
    if (m_client->m_socket == nullptr) {
        m_client->m_http_session.reset();
        m_client->m_connected = false;
        return;
    }
    start_io(m_client->m_socket->close());
}

bool details::CloseAwaitable::await_ready() const noexcept
{
    return IoAwaitableBase::await_ready();
}

EtcdBoolResult details::CloseAwaitable::await_resume()
{
    EtcdBoolResult result = true;
    auto& io_awaitable = awaitable();
    if (io_awaitable.has_value()) {
        auto close_result = io_awaitable->await_resume();
        if (!close_result.has_value()) {
            EtcdError error = map_kernel_io_error(close_result.error());
            m_client->set_error(error);
            ETCD_LOG_ERROR("[async] [close]", "close failed endpoint={} error={}",
                           m_client->m_config.endpoint,
                           error.message());
            result = std::unexpected(error);
        }
    } else {
        result = m_client->current_bool_result();
    }

    m_client->stop_watch_workers();
    m_client->m_http_session.reset();
    m_client->m_socket.reset();
    m_client->m_connected = false;
    ETCD_LOG_INFO("[async] [close]", "closed endpoint={}", m_client->m_config.endpoint);
    return result;
}

details::GetAwaitable::GetAwaitable(AsyncEtcdClient& client,
                                   std::string key,
                                   bool prefix,
                                   std::optional<int64_t> limit)
    : JsonOpAwaitableBase(client)
{
    m_client->reset_last_operation();
    auto body = build_get_request_body(key, prefix, limit);
    if (!body.has_value()) {
        m_client->set_error(body.error());
        return;
    }

    start_post("/kv/range", std::move(body.value()));
}

bool details::GetAwaitable::await_ready() const noexcept
{
    return JsonOpAwaitableBase::await_ready();
}

EtcdGetResult details::GetAwaitable::await_resume()
{
    auto response_body = resume_post();
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto kvs_result = parse_get_response_kvs(response_body.value());
    if (!kvs_result.has_value()) {
        m_client->set_error(kvs_result.error());
        return std::unexpected(kvs_result.error());
    }

    return kvs_result.value();
}

details::DeleteAwaitable::DeleteAwaitable(AsyncEtcdClient& client,
                                         std::string key,
                                         bool prefix)
    : JsonOpAwaitableBase(client)
{
    m_client->reset_last_operation();
    auto body = build_delete_request_body(key, prefix);
    if (!body.has_value()) {
        m_client->set_error(body.error());
        return;
    }

    start_post("/kv/deleterange", std::move(body.value()));
}

bool details::DeleteAwaitable::await_ready() const noexcept
{
    return JsonOpAwaitableBase::await_ready();
}

EtcdDeleteResult details::DeleteAwaitable::await_resume()
{
    auto response_body = resume_post();
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto deleted_result = parse_delete_response_deleted_count(response_body.value());
    if (!deleted_result.has_value()) {
        m_client->set_error(deleted_result.error());
        return std::unexpected(deleted_result.error());
    }
    return deleted_result.value();
}

details::GrantLeaseAwaitable::GrantLeaseAwaitable(
    AsyncEtcdClient& client,
    int64_t ttl_seconds)
    : JsonOpAwaitableBase(client)
{
    m_client->reset_last_operation();
    auto body = build_lease_grant_request_body(ttl_seconds);
    if (!body.has_value()) {
        m_client->set_error(body.error());
        return;
    }

    start_post("/lease/grant", std::move(body.value()));
}

bool details::GrantLeaseAwaitable::await_ready() const noexcept
{
    return JsonOpAwaitableBase::await_ready();
}

EtcdLeaseGrantResult details::GrantLeaseAwaitable::await_resume()
{
    auto response_body = resume_post();
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto lease_result = parse_lease_grant_response_id(response_body.value());
    if (!lease_result.has_value()) {
        m_client->set_error(lease_result.error());
        return std::unexpected(lease_result.error());
    }
    return lease_result.value();
}

details::KeepAliveAwaitable::KeepAliveAwaitable(
    AsyncEtcdClient& client,
    int64_t lease_id)
    : JsonOpAwaitableBase(client)
    , m_lease_id(lease_id)
{
    m_client->reset_last_operation();
    auto body = build_lease_keep_alive_request_body(m_lease_id);
    if (!body.has_value()) {
        m_client->set_error(body.error());
        return;
    }

    std::optional<std::chrono::milliseconds> timeout = std::nullopt;
    if (!m_client->m_network_config.is_request_timeout_enabled()) {
        timeout = std::chrono::seconds(5);
    }

    start_post("/lease/keepalive", std::move(body.value()), timeout);
}

bool details::KeepAliveAwaitable::await_ready() const noexcept
{
    return JsonOpAwaitableBase::await_ready();
}

EtcdLeaseGrantResult details::KeepAliveAwaitable::await_resume()
{
    auto response_body = resume_post();
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto keepalive_result = parse_lease_keep_alive_response_id(response_body.value(), m_lease_id);
    if (!keepalive_result.has_value()) {
        m_client->set_error(keepalive_result.error());
        return std::unexpected(keepalive_result.error());
    }

    return keepalive_result.value();
}

details::PipelineAwaitable::PipelineAwaitable(AsyncEtcdClient& client,
                                              std::span<const PipelineOp> operations)
    : JsonOpAwaitableBase(client)
{
    m_client->reset_last_operation();
    m_operation_types.reserve(operations.size());
    for (const auto& op : operations) {
        m_operation_types.push_back(op.type);
    }

    auto body = build_txn_body(operations);
    if (!body.has_value()) {
        m_client->set_error(body.error());
        return;
    }
    start_post("/kv/txn", std::move(body.value()));
}

details::PipelineAwaitable::PipelineAwaitable(AsyncEtcdClient& client,
                                              std::vector<PipelineOp> operations)
    : PipelineAwaitable(client, std::span<const PipelineOp>(operations.data(), operations.size()))
{
}

bool details::PipelineAwaitable::await_ready() const noexcept
{
    return JsonOpAwaitableBase::await_ready();
}

EtcdPipelineResult details::PipelineAwaitable::await_resume()
{
    auto response_body = resume_post();
    if (!response_body.has_value()) {
        return std::unexpected(response_body.error());
    }

    auto pipeline_results = parse_pipeline_txn_response(
        response_body.value(),
        std::span<const PipelineOpType>(m_operation_types.data(), m_operation_types.size()));
    if (!pipeline_results.has_value()) {
        m_client->set_error(pipeline_results.error());
        return std::unexpected(pipeline_results.error());
    }

    return pipeline_results.value();
}
