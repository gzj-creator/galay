#include <galay/cpp/galay-postgres/protoc/postgres_auth.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace galay::postgres::protocol;

namespace
{

constexpr std::string_view kClientNonce = "rOprNGfwEbeRWgbNEkqO";
constexpr std::string_view kServerNonce =
    "rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0";
constexpr std::string_view kServerFirst =
    "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
    "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
constexpr std::string_view kExpectedClientFinal =
    "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
    "p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=";
constexpr std::string_view kExpectedServerFinal =
    "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=";

void require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

ScramSha256 make_ready_for_final()
{
    ScramSha256 scram;
    auto first = scram.client_first_message("user", kClientNonce);
    require(first && *first == "n,,n=user,r=rOprNGfwEbeRWgbNEkqO",
            "RFC 7677 client-first-message mismatch");
    auto server = scram.parse_server_first(kServerFirst);
    require(server.has_value(), "RFC 7677 server-first-message rejected");
    auto final = scram.client_final_message("pencil");
    require(final && *final == kExpectedClientFinal, "RFC 7677 client proof mismatch");
    return scram;
}

void test_postgres_client_first_form_and_escaping()
{
    ScramSha256 scram;
    auto postgres_first = scram.client_first_message("", "abcdefghijklmnopqrstuvwx");
    require(postgres_first && *postgres_first == "n,,n=,r=abcdefghijklmnopqrstuvwx",
            "PostgreSQL SCRAM must use the empty SASL username form");

    scram.reset();
    auto escaped = scram.client_first_message("a,b=c", "abcdefghijklmnopqrstuvwx");
    require(escaped && *escaped == "n,,n=a=2Cb=3Dc,r=abcdefghijklmnopqrstuvwx",
            "SCRAM username escaping mismatch");

    scram.reset();
    require(!scram.client_first_message("user", ""), "empty client nonce must fail");
    require(!scram.client_first_message("user", "bad,nonce"), "comma in client nonce must fail");
    require(!scram.client_first_message("bad\nuser", "abcdefghijklmnopqrstuvwx"),
            "control characters in username must fail");

    auto nonce = ScramSha256::generate_nonce();
    require(nonce && nonce->size() == 24, "generated SCRAM nonce must encode 18 random bytes");
    require(nonce->find(',') == std::string::npos, "generated nonce contains a forbidden comma");
    require(nonce->find('=') == std::string::npos, "18-byte nonce must not require Base64 padding");
}

void test_rfc7677_vector_and_server_verification()
{
    ScramSha256 scram = make_ready_for_final();
    auto verified = scram.verify_server_final(kExpectedServerFinal);
    require(verified.has_value(), "RFC 7677 server signature mismatch");

    ScramSha256 wrong = make_ready_for_final();
    auto wrong_result = wrong.verify_server_final(
        "v=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
    require(!wrong_result && wrong_result.error().find("signature") != std::string::npos,
            "wrong server signature must fail authentication");

    ScramSha256 server_error = make_ready_for_final();
    auto error_result = server_error.verify_server_final("e=invalid-proof");
    require(!error_result && error_result.error().find("invalid-proof") != std::string::npos,
            "SCRAM server error must be propagated");
}

void test_strict_server_first_parsing()
{
    const std::vector<std::string> malformed{
        "",
        "r=othernonce,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096",
        "r=rOprNGfwEbeRWgbNEkqO,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096",
        "s=W22ZaJ0SNY7soEsUEjb6gQ==,r=rOprNGfwEbeRWgbNEkqOserver,i=4096",
        "r=rOprNGfwEbeRWgbNEkqOserver,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=0",
        "r=rOprNGfwEbeRWgbNEkqOserver,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096x",
        "r=rOprNGfwEbeRWgbNEkqOserver,s=YW=J,i=4096",
        "r=rOprNGfwEbeRWgbNEkqOserver,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096,x=extra",
        "m=extension,r=rOprNGfwEbeRWgbNEkqOserver,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096",
    };

    for (const std::string& message : malformed) {
        ScramSha256 scram;
        require(scram.client_first_message("user", kClientNonce).has_value(),
                "failed to initialize strict parser case");
        auto parsed = scram.parse_server_first(message);
        require(!parsed, "malformed server-first-message unexpectedly accepted");
    }
}

void test_state_ordering_and_strict_server_final_parsing()
{
    ScramSha256 scram;
    require(!scram.parse_server_first(kServerFirst),
            "server-first-message before client-first-message must fail");
    require(!scram.client_final_message("pencil"),
            "client-final-message before server-first-message must fail");
    require(!scram.verify_server_final(kExpectedServerFinal),
            "server-final-message before client proof must fail");

    const std::vector<std::string> malformed_final{
        "",
        "v=YW=J",
        "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=,x=extra",
        "e=bad,v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=",
    };
    for (const std::string& message : malformed_final) {
        ScramSha256 ready = make_ready_for_final();
        require(!ready.verify_server_final(message),
                "malformed server-final-message unexpectedly accepted");
    }
}

} // namespace

int main()
{
    test_postgres_client_first_form_and_escaping();
    test_rfc7677_vector_and_server_verification();
    test_strict_server_first_parsing();
    test_state_ordering_and_strict_server_final_parsing();
    return EXIT_SUCCESS;
}
