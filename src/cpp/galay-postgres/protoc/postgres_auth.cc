#include "postgres_auth.h"

#include <galay/cpp/galay-utils/crypto/hmac.hpp>
#include <galay/cpp/galay-utils/crypto/md5.hpp>
#include <galay/cpp/galay-utils/crypto/pbkdf2.hpp>
#include <chrono>
#include <galay/cpp/galay-utils/crypto/salt.hpp>
#include <galay/cpp/galay-utils/encoding/base64.hpp>

#include <algorithm>
#include <charconv>
#include <limits>

namespace galay::postgres::protocol
{

namespace
{

bool isValidNonce(std::string_view nonce)
{
    if (nonce.empty()) {
        return false;
    }
    for (unsigned char value : nonce) {
        if (value < 0x21 || value > 0x7e || value == ',') {
            return false;
        }
    }
    return true;
}

std::expected<std::string, std::string> escapeUsername(std::string_view username)
{
    std::string escaped;
    escaped.reserve(username.size());
    for (unsigned char value : username) {
        if (value < 0x20 || value == 0x7f) {
            return std::unexpected("SCRAM username contains a control character");
        }
        if (value == ',') {
            escaped.append("=2C");
        } else if (value == '=') {
            escaped.append("=3D");
        } else {
            escaped.push_back(static_cast<char>(value));
        }
    }
    return escaped;
}

std::string base64Encode(std::span<const uint8_t> bytes)
{
    return galay::utils::Base64Util::Base64Encode(bytes.data(), bytes.size());
}

std::expected<std::vector<uint8_t>, std::string>
strictBase64Decode(std::string_view encoded)
{
    if (encoded.empty() || encoded.size() % 4 != 0) {
        return std::unexpected("invalid Base64 length");
    }

    size_t padding = 0;
    if (encoded.ends_with("==")) {
        padding = 2;
    } else if (encoded.ends_with('=')) {
        padding = 1;
    }
    for (size_t index = 0; index < encoded.size(); ++index) {
        const unsigned char value = static_cast<unsigned char>(encoded[index]);
        const bool is_alpha_numeric =
            (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9');
        const bool is_data = is_alpha_numeric || value == '+' || value == '/';
        const bool is_padding = value == '=' && index >= encoded.size() - padding;
        if (!is_data && !is_padding) {
            return std::unexpected("invalid Base64 character or padding");
        }
    }

    if (!galay::utils::Base64Util::Base64CanDecodeView(encoded)) {
        return std::unexpected("invalid Base64 input");
    }
    const std::string decoded = galay::utils::Base64Util::Base64DecodeView(encoded);
    if (decoded.empty()) {
        return std::unexpected("empty Base64 value");
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(decoded.data());
    if (base64Encode(std::span<const uint8_t>(bytes, decoded.size())) != encoded) {
        return std::unexpected("non-canonical Base64 input");
    }
    return std::vector<uint8_t>(bytes, bytes + decoded.size());
}

} // namespace

std::expected<std::string, std::string> ScramSha256::generateNonce()
{
    const std::vector<uint8_t> bytes = galay::utils::SaltGenerator::generateSecureBytes(18);
    if (bytes.size() != 18) {
        return std::unexpected("failed to generate SCRAM nonce bytes");
    }
    std::string nonce = base64Encode(bytes);
    if (!isValidNonce(nonce)) {
        return std::unexpected("failed to encode a valid SCRAM nonce");
    }
    return nonce;
}

std::expected<std::string, std::string>
ScramSha256::clientFirstMessage(std::string_view username, std::string_view nonce)
{
    if (m_phase != Phase::Initial) {
        return std::unexpected("SCRAM client-first-message is out of order");
    }
    if (!isValidNonce(nonce)) {
        return std::unexpected("invalid SCRAM client nonce");
    }
    auto escaped_username = escapeUsername(username);
    if (!escaped_username) {
        return std::unexpected(escaped_username.error());
    }

    m_client_nonce.assign(nonce);
    m_client_first_bare = "n=" + *escaped_username + ",r=" + m_client_nonce;
    m_phase = Phase::ClientFirstSent;
    return "n,," + m_client_first_bare;
}

std::expected<void, std::string>
ScramSha256::parseServerFirst(std::string_view server_first)
{
    if (m_phase != Phase::ClientFirstSent) {
        return std::unexpected("SCRAM server-first-message is out of order");
    }
    if (server_first.starts_with("m=")) {
        return std::unexpected("unsupported mandatory SCRAM extension");
    }

    const size_t first_comma = server_first.find(',');
    const size_t second_comma = first_comma == std::string_view::npos
        ? std::string_view::npos
        : server_first.find(',', first_comma + 1);
    if (first_comma == std::string_view::npos || second_comma == std::string_view::npos ||
        server_first.find(',', second_comma + 1) != std::string_view::npos) {
        return std::unexpected("malformed SCRAM server-first-message");
    }

    const std::string_view nonce_attribute = server_first.substr(0, first_comma);
    const std::string_view salt_attribute =
        server_first.substr(first_comma + 1, second_comma - first_comma - 1);
    const std::string_view iteration_attribute = server_first.substr(second_comma + 1);
    if (!nonce_attribute.starts_with("r=") || !salt_attribute.starts_with("s=") ||
        !iteration_attribute.starts_with("i=")) {
        return std::unexpected("unexpected SCRAM server-first attribute order");
    }

    const std::string_view server_nonce = nonce_attribute.substr(2);
    if (!isValidNonce(server_nonce) || !server_nonce.starts_with(m_client_nonce) ||
        server_nonce.size() <= m_client_nonce.size()) {
        return std::unexpected("SCRAM server nonce does not extend the client nonce");
    }

    auto salt = strictBase64Decode(salt_attribute.substr(2));
    if (!salt) {
        return std::unexpected("invalid SCRAM salt: " + salt.error());
    }

    const std::string_view iteration_text = iteration_attribute.substr(2);
    uint32_t iterations = 0;
    const auto parsed = std::from_chars(iteration_text.data(),
                                        iteration_text.data() + iteration_text.size(),
                                        iterations);
    if (iteration_text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != iteration_text.data() + iteration_text.size() || iterations == 0) {
        return std::unexpected("invalid SCRAM iteration count");
    }

    m_server_first.assign(server_first);
    m_server_nonce.assign(server_nonce);
    m_salt = std::move(*salt);
    m_iterations = iterations;
    m_phase = Phase::ServerFirstReceived;
    return {};
}

std::expected<std::string, std::string>
ScramSha256::clientFinalMessage(std::string_view password)
{
    if (m_phase != Phase::ServerFirstReceived) {
        return std::unexpected("SCRAM client-final-message is out of order");
    }

    const auto* password_bytes = reinterpret_cast<const uint8_t*>(password.data());
    std::vector<uint8_t> salted_password = galay::utils::PBKDF2::hmacSha256(
        password_bytes,
        password.size(),
        m_salt.data(),
        m_salt.size(),
        m_iterations,
        32);
    if (salted_password.size() != 32) {
        return std::unexpected("PBKDF2-HMAC-SHA256 failed");
    }

    const std::string client_final_without_proof = "c=biws,r=" + m_server_nonce;
    const std::string auth_message = m_client_first_bare + "," + m_server_first + "," +
                                     client_final_without_proof;
    static constexpr std::string_view kClientKeyLabel = "Client Key";
    static constexpr std::string_view kServerKeyLabel = "Server Key";

    const auto client_key = galay::utils::HMAC::hmacSha256(
        salted_password.data(), salted_password.size(),
        reinterpret_cast<const uint8_t*>(kClientKeyLabel.data()), kClientKeyLabel.size());
    const auto stored_key = galay::utils::SHA256::hash(client_key.data(), client_key.size());
    const auto client_signature = galay::utils::HMAC::hmacSha256(
        stored_key.data(), stored_key.size(),
        reinterpret_cast<const uint8_t*>(auth_message.data()), auth_message.size());

    std::array<uint8_t, 32> client_proof{};
    for (size_t index = 0; index < client_proof.size(); ++index) {
        client_proof[index] = static_cast<uint8_t>(client_key[index] ^ client_signature[index]);
    }

    const auto server_key = galay::utils::HMAC::hmacSha256(
        salted_password.data(), salted_password.size(),
        reinterpret_cast<const uint8_t*>(kServerKeyLabel.data()), kServerKeyLabel.size());
    m_expected_server_signature = galay::utils::HMAC::hmacSha256(
        server_key.data(), server_key.size(),
        reinterpret_cast<const uint8_t*>(auth_message.data()), auth_message.size());

    std::fill(salted_password.begin(), salted_password.end(), uint8_t{0});
    m_phase = Phase::ClientFinalSent;
    return client_final_without_proof + ",p=" + base64Encode(client_proof);
}

std::expected<void, std::string>
ScramSha256::verifyServerFinal(std::string_view server_final)
{
    if (m_phase != Phase::ClientFinalSent) {
        return std::unexpected("SCRAM server-final-message is out of order");
    }
    if (server_final.empty() || server_final.find(',') != std::string_view::npos) {
        return std::unexpected("malformed SCRAM server-final-message");
    }
    if (server_final.starts_with("e=")) {
        const std::string_view server_error = server_final.substr(2);
        return std::unexpected(server_error.empty()
            ? std::string("empty SCRAM server error")
            : std::string("SCRAM server error: ") + std::string(server_error));
    }
    if (!server_final.starts_with("v=")) {
        return std::unexpected("SCRAM server-final-message is missing the verifier");
    }

    auto verifier = strictBase64Decode(server_final.substr(2));
    if (!verifier || verifier->size() != m_expected_server_signature.size()) {
        return std::unexpected("invalid SCRAM server signature");
    }

    uint8_t difference = 0;
    for (size_t index = 0; index < m_expected_server_signature.size(); ++index) {
        difference |= static_cast<uint8_t>((*verifier)[index] ^
                                           m_expected_server_signature[index]);
    }
    if (difference != 0) {
        return std::unexpected("SCRAM server signature mismatch");
    }
    m_phase = Phase::Verified;
    return {};
}

void ScramSha256::reset() noexcept
{
    std::fill(m_expected_server_signature.begin(), m_expected_server_signature.end(), uint8_t{0});
    std::fill(m_salt.begin(), m_salt.end(), uint8_t{0});
    m_client_nonce.clear();
    m_client_first_bare.clear();
    m_server_first.clear();
    m_server_nonce.clear();
    m_salt.clear();
    m_iterations = 0;
    m_phase = Phase::Initial;
}

std::string md5Password(std::string_view username,
                        std::string_view password,
                        std::span<const uint8_t, 4> salt)
{
    std::string first_input;
    first_input.reserve(password.size() + username.size());
    first_input.append(password);
    first_input.append(username);
    const std::string first_digest = galay::utils::MD5Util::MD5View(first_input);

    std::string second_input = first_digest;
    second_input.append(reinterpret_cast<const char*>(salt.data()), salt.size());
    return "md5" + galay::utils::MD5Util::MD5View(second_input);
}

} // namespace galay::postgres::protocol
