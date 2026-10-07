#include "client.h"
#include "http_transport.h"
#include "stdio_transport.h"

namespace galay::mcp {

namespace {

template <typename T>
galay::kernel::Task<void> make_wrong_mode_task(std::expected<T, McpError>& result, std::string_view message) {
    result = std::unexpected(McpError::invalid_transport_mode(std::string(message)));
    co_return;
}

McpClient::ConnectAwaitable make_immediate_connect_error_task() {
    co_return std::unexpected(::galay::kernel::IOError(::galay::kernel::kParamInvalid, 0));
}

McpClient::CloseAwaitable make_immediate_close_error_task() {
    co_return std::unexpected(::galay::kernel::IOError(::galay::kernel::kParamInvalid, 0));
}

} // namespace

class McpClient::Impl {
public:
    explicit Impl(McpStdioClientConfig config)
        : stdioTransport(std::make_unique<detail::StdioClientTransport>(config.input, config.output))
        , mode(McpClientMode::Stdio) {}

    Impl(kernel::Runtime& runtime, McpHttpClientConfig config)
        : httpTransport(std::make_unique<detail::HttpClientTransport>(runtime, std::move(config)))
        , mode(McpClientMode::Http) {}

    std::unique_ptr<detail::StdioClientTransport> stdioTransport;
    std::unique_ptr<detail::HttpClientTransport> httpTransport;
    McpClientMode mode;
};

McpClient::McpClient(McpStdioClientConfig config)
    : m_impl(std::make_unique<Impl>(config)) {
}

McpClient::McpClient(kernel::Runtime& runtime, McpHttpClientConfig config)
    : m_impl(std::make_unique<Impl>(runtime, std::move(config))) {
}

McpClient::~McpClient() = default;

McpClientMode McpClient::mode() const {
    return m_impl->mode;
}

McpClient::ConnectAwaitable McpClient::connect() {
    if (m_impl->mode != McpClientMode::Http) {
        return make_immediate_connect_error_task();
    }
    return m_impl->httpTransport->connect();
}

McpClient::ConnectAwaitable McpClient::connect(std::string url) {
    if (m_impl->mode != McpClientMode::Http) {
        return make_immediate_connect_error_task();
    }
    return m_impl->httpTransport->connect(std::move(url));
}

McpClient::CloseAwaitable McpClient::disconnect_async() {
    if (m_impl->mode != McpClientMode::Http) {
        return make_immediate_close_error_task();
    }
    return m_impl->httpTransport->disconnect_async();
}

std::expected<void, McpError> McpClient::disconnect() {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalid_transport_mode("stdio disconnect called on HTTP client"));
    }
    return m_impl->stdioTransport->disconnect();
}

std::expected<void, McpError> McpClient::initialize(const std::string& clientName,
                                                   const std::string& clientVersion) {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalid_transport_mode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->initialize(clientName, clientVersion);
}

std::expected<std::string, McpError> McpClient::call_tool(const std::string& toolName,
                                                        const std::string& arguments) {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalid_transport_mode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->call_tool(toolName, arguments);
}

std::expected<std::vector<Tool>, McpError> McpClient::list_tools() {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalid_transport_mode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->list_tools();
}

std::expected<std::vector<Resource>, McpError> McpClient::list_resources() {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalid_transport_mode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->list_resources();
}

std::expected<std::string, McpError> McpClient::read_resource(const std::string& uri) {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalid_transport_mode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->read_resource(uri);
}

std::expected<std::vector<Prompt>, McpError> McpClient::list_prompts() {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalid_transport_mode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->list_prompts();
}

std::expected<std::string, McpError> McpClient::get_prompt(const std::string& name,
                                                         const std::string& arguments) {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalid_transport_mode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->get_prompt(name, arguments);
}

std::expected<void, McpError> McpClient::ping() {
    if (m_impl->mode != McpClientMode::Stdio) {
        return std::unexpected(McpError::invalid_transport_mode("stdio API called on HTTP client"));
    }
    return m_impl->stdioTransport->ping();
}

galay::kernel::Task<void> McpClient::initialize(std::string clientName,
                                std::string clientVersion,
                                std::expected<void, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await make_wrong_mode_task(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->initialize(std::move(clientName), std::move(clientVersion), result);
}

galay::kernel::Task<void> McpClient::call_tool(std::string toolName,
                              std::string arguments,
                              std::expected<std::string, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await make_wrong_mode_task(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->call_tool(std::move(toolName), std::move(arguments), result);
}

galay::kernel::Task<void> McpClient::list_tools(std::expected<std::vector<Tool>, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await make_wrong_mode_task(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->list_tools(result);
}

galay::kernel::Task<void> McpClient::list_resources(std::expected<std::vector<Resource>, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await make_wrong_mode_task(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->list_resources(result);
}

galay::kernel::Task<void> McpClient::read_resource(std::string uri,
                                  std::expected<std::string, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await make_wrong_mode_task(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->read_resource(std::move(uri), result);
}

galay::kernel::Task<void> McpClient::list_prompts(std::expected<std::vector<Prompt>, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await make_wrong_mode_task(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->list_prompts(result);
}

galay::kernel::Task<void> McpClient::get_prompt(std::string name,
                               std::string arguments,
                               std::expected<std::string, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await make_wrong_mode_task(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->get_prompt(std::move(name), std::move(arguments), result);
}

galay::kernel::Task<void> McpClient::ping(std::expected<void, McpError>& result) {
    if (m_impl->mode != McpClientMode::Http) {
        co_await make_wrong_mode_task(result, "HTTP API called on stdio client");
        co_return;
    }
    co_await m_impl->httpTransport->ping(result);
}

bool McpClient::is_connected() const {
    if (m_impl->mode == McpClientMode::Stdio) {
        return m_impl->stdioTransport->is_connected();
    }
    return m_impl->httpTransport->is_connected();
}

bool McpClient::is_initialized() const {
    if (m_impl->mode == McpClientMode::Stdio) {
        return m_impl->stdioTransport->is_initialized();
    }
    return m_impl->httpTransport->is_initialized();
}

const ServerInfo& McpClient::get_server_info() const {
    if (m_impl->mode == McpClientMode::Stdio) {
        return m_impl->stdioTransport->get_server_info();
    }
    return m_impl->httpTransport->get_server_info();
}

const ServerCapabilities& McpClient::get_server_capabilities() const {
    if (m_impl->mode == McpClientMode::Stdio) {
        return m_impl->stdioTransport->get_server_capabilities();
    }
    return m_impl->httpTransport->get_server_capabilities();
}

} // namespace galay::mcp
