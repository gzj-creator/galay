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

ScramSha256 makeReadyForFinal()
{
    ScramSha256 scram;
    auto first = scram.clientFirstMessage("user", kClientNonce);
    require(first && *first == "n,,n=user,r=rOprNGfwEbeRWgbNEkqO",
            "RFC 7677 client-first-message mismatch");
    auto server = scram.parseServerFirst(kServerFirst);
    require(server.has_value(), "RFC 7677 server-first-message rejected");
    auto final = scram.clientFinalMessage("pencil");
    require(final && *final == kExpectedClientFinal, "RFC 7677 client proof mismatch");
    return scram;
}

void testPostgresClientFirstFormAndEscaping()
{
    ScramSha256 scram;
    auto postgres_first = scram.clientFirstMessage("", "abcdefghijklmnopqrstuvwx");
    require(postgres_first && *postgres_first == "n,,n=,r=abcdefghijklmnopqrstuvwx",
            "PostgreSQL SCRAM must use the empty SASL username form");

    scram.reset();
    auto escaped = scram.clientFirstMessage("a,b=c", "abcdefghijklmnopqrstuvwx");
    require(escaped && *escaped == "n,,n=a=2Cb=3Dc,r=abcdefghijklmnopqrstuvwx",
            "SCRAM username escaping mismatch");

    scram.reset();
    require(!scram.clientFirstMessage("user", ""), "empty client nonce must fail");
    require(!scram.clientFirstMessage("user", "bad,nonce"), "comma in client nonce must fail");
    require(!scram.clientFirstMessage("bad\nuser", "abcdefghijklmnopqrstuvwx"),
            "control characters in username must fail");

    auto nonce = ScramSha256::generateNonce();
    require(nonce && nonce->size() == 24, "generated SCRAM nonce must encode 18 random bytes");
    require(nonce->find(',') == std::string::npos, "generated nonce contains a forbidden comma");
    require(nonce->find('=') == std::string::npos, "18-byte nonce must not require Base64 padding");
}

void testRfc7677VectorAndServerVerification()
{
    ScramSha256 scram = makeReadyForFinal();
    auto verified = scram.verifyServerFinal(kExpectedServerFinal);
    require(verified.has_value(), "RFC 7677 server signature mismatch");

    ScramSha256 wrong = makeReadyForFinal();
    auto wrong_result = wrong.verifyServerFinal(
        "v=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
    require(!wrong_result && wrong_result.error().find("signature") != std::string::npos,
            "wrong server signature must fail authentication");

    ScramSha256 server_error = makeReadyForFinal();
    auto error_result = server_error.verifyServerFinal("e=invalid-proof");
    require(!error_result && error_result.error().find("invalid-proof") != std::string::npos,
            "SCRAM server error must be propagated");
}

void testStrictServerFirstParsing()
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
        require(scram.clientFirstMessage("user", kClientNonce).has_value(),
                "failed to initialize strict parser case");
        auto parsed = scram.parseServerFirst(message);
        require(!parsed, "malformed server-first-message unexpectedly accepted");
    }
}

void testStateOrderingAndStrictServerFinalParsing()
{
    ScramSha256 scram;
    require(!scram.parseServerFirst(kServerFirst),
            "server-first-message before client-first-message must fail");
    require(!scram.clientFinalMessage("pencil"),
            "client-final-message before server-first-message must fail");
    require(!scram.verifyServerFinal(kExpectedServerFinal),
            "server-final-message before client proof must fail");

    const std::vector<std::string> malformed_final{
        "",
        "v=YW=J",
        "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=,x=extra",
        "e=bad,v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=",
    };
    for (const std::string& message : malformed_final) {
        ScramSha256 ready = makeReadyForFinal();
        require(!ready.verifyServerFinal(message),
                "malformed server-final-message unexpectedly accepted");
    }
}

} // namespace

int main()
{
    testPostgresClientFirstFormAndEscaping();
    testRfc7677VectorAndServerVerification();
    testStrictServerFirstParsing();
    testStateOrderingAndStrictServerFinalParsing();
    return EXIT_SUCCESS;
}
