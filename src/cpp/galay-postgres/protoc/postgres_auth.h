/**
 * @file postgres_auth.h
 * @brief PostgreSQL SCRAM-SHA-256 and MD5 authentication helpers.
 */

#ifndef GALAY_POSTGRES_AUTH_H
#define GALAY_POSTGRES_AUTH_H

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace galay::postgres::protocol
{

/**
 * @brief Stateful SCRAM-SHA-256 client exchange.
 * @details Methods must be called in client-first, server-first, client-final,
 *          server-final order. Password bytes are used as supplied; this helper
 *          does not provide Unicode SASLprep normalization.
 */
class ScramSha256
{
public:
    ScramSha256() = default;
    ScramSha256(ScramSha256&&) noexcept = default;
    ScramSha256& operator=(ScramSha256&&) noexcept = default;
    ScramSha256(const ScramSha256&) = delete;
    ScramSha256& operator=(const ScramSha256&) = delete;

    /** Generate the PostgreSQL/libpq-style Base64 encoding of 18 random bytes. */
    [[nodiscard]] static std::expected<std::string, std::string> generateNonce();

    /**
     * PostgreSQL already sends the role in StartupMessage, so production
     * connections pass an empty username here and emit the standard `n=` form.
     * A non-empty username remains supported for RFC 7677 vector verification.
     */
    [[nodiscard]] std::expected<std::string, std::string>
    clientFirstMessage(std::string_view username, std::string_view nonce);
    [[nodiscard]] std::expected<void, std::string>
    parseServerFirst(std::string_view server_first);
    [[nodiscard]] std::expected<std::string, std::string>
    clientFinalMessage(std::string_view password);
    [[nodiscard]] std::expected<void, std::string>
    verifyServerFinal(std::string_view server_final);

    void reset() noexcept;

private:
    enum class Phase
    {
        Initial,
        ClientFirstSent,
        ServerFirstReceived,
        ClientFinalSent,
        Verified,
    };

    std::string m_client_nonce;
    std::string m_client_first_bare;
    std::string m_server_first;
    std::string m_server_nonce;
    std::vector<uint8_t> m_salt;
    std::array<uint8_t, 32> m_expected_server_signature{};
    uint32_t m_iterations = 0;
    Phase m_phase = Phase::Initial;
};

[[nodiscard]] std::string md5Password(std::string_view username,
                                      std::string_view password,
                                      std::span<const uint8_t, 4> salt);

} // namespace galay::postgres::protocol

#endif // GALAY_POSTGRES_AUTH_H
