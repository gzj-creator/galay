#include "test_common.hpp"

void testParser() {
    std::cout << "=== Testing Parser ===" << std::endl;

    // Config parser
    ConfigParser config;
    std::string configContent = R"(
# Comment
[database]
host = localhost
port = 5432
name = "test_db"

[server]
port = 8080
debug = true
)";

    assert(config.parseString(configContent));

    assert(config.getValue("database.host").value() == "localhost");
    assert(config.getValueAs<int>("database.port", 0) == 5432);
    assert(config.getValue("database.name").value() == "test_db");
    assert(config.getValueAs<int>("server.port", 0) == 8080);

    auto dbKeys = config.getKeysInSection("database");
    assert(dbKeys.size() == 3);

    // INI parser
    IniParser ini;
    assert(ini.parseString(configContent));
    assert(ini.getValue("database.host").value() == "localhost");
    assert(ini.getValueAs<int>("server.port", 0) == 8080);

    // Env parser
    EnvParser env;
    std::string envContent = R"(
# Environment variables
DATABASE_URL=postgres://localhost/db
export API_KEY=secret123
DEBUG=true
)";

    assert(env.parseString(envContent));
    assert(env.getValue("DATABASE_URL").value() == "postgres://localhost/db");
    assert(env.getValue("API_KEY").value() == "secret123");
    assert(env.getValue("DEBUG").value() == "true");

    // TOML parser
    TomlParser toml;
    std::string tomlContent = R"(
title = "galay-utils"
enabled = true
ports = [8000, 8001, 8002]

[database]
host = "localhost"
port = 5432
ratio = 0.75
tags = ["primary", "readonly"]
)";

    assert(toml.parseString(tomlContent));
    assert(toml.getValue("title").value() == "galay-utils");
    assert(toml.getValue("enabled").value() == "true");
    assert(toml.getValueAs<int>("database.port", 0) == 5432);
    assert(toml.getValue("database.ratio").value() == "0.75");

    auto ports = toml.getArray("ports");
    assert(ports.size() == 3 && ports[0] == "8000" && ports[2] == "8002");

    auto tags = toml.getArray("database.tags");
    assert(tags.size() == 2 && tags[0] == "primary" && tags[1] == "readonly");

    TomlParser tomlMultilineArray;
    std::string tomlMultilineArrayContent = R"(
ports = [
    8000,
    8001,
    8002,
]

[database]
tags = [
    "primary",
    "readonly",
]
)";

    assert(tomlMultilineArray.parseString(tomlMultilineArrayContent));
    auto multilinePorts = tomlMultilineArray.getArray("ports");
    assert(multilinePorts.size() == 3 && multilinePorts[0] == "8000" && multilinePorts[2] == "8002");

    auto multilineTags = tomlMultilineArray.getArray("database.tags");
    assert(multilineTags.size() == 2 && multilineTags[0] == "primary" && multilineTags[1] == "readonly");

    TomlParser tomlTrailingCommaArray;
    assert(tomlTrailingCommaArray.parseString("ports = [7000, 7001,]"));
    auto trailingCommaPorts = tomlTrailingCommaArray.getArray("ports");
    assert(trailingCommaPorts.size() == 2 && trailingCommaPorts[0] == "7000" && trailingCommaPorts[1] == "7001");

    TomlParser tomlCommentedMultilineArray;
    std::string tomlCommentedMultilineArrayContent = R"(
ports = [ # opening comment
    8000, # first port

    8001
] # closing comment
)";

    assert(tomlCommentedMultilineArray.parseString(tomlCommentedMultilineArrayContent));
    auto commentedPorts = tomlCommentedMultilineArray.getArray("ports");
    assert(commentedPorts.size() == 2 && commentedPorts[0] == "8000" && commentedPorts[1] == "8001");

    TomlParser tomlQuotedMultilineArray;
    std::string tomlQuotedMultilineArrayContent = R"(
tags = [
    "literal ] # not comment",
    "a,b",
    'C:\tmp\',
]
)";

    assert(tomlQuotedMultilineArray.parseString(tomlQuotedMultilineArrayContent));
    auto quotedTags = tomlQuotedMultilineArray.getArray("tags");
    assert(quotedTags.size() == 3);
    assert(quotedTags[0] == "literal ] # not comment");
    assert(quotedTags[1] == "a,b");
    assert(quotedTags[2] == R"(C:\tmp\)");

    TomlParser tomlLiteralString;
    assert(tomlLiteralString.parseString(R"(path = 'C:\tmp\' # keep literal backslash)"));
    assert(tomlLiteralString.getValue("path").value() == R"(C:\tmp\)");

    auto tomlParser = ParserManager::instance().createParser("config.toml");
    assert(tomlParser != nullptr);
    assert(tomlParser->parseString(tomlContent));
    assert(tomlParser->getValue("database.host").value() == "localhost");

    TomlParser tomlEdge;
    std::string tomlEdgeContent = R"(
# full-line comment
title = "value # not comment" # inline comment
path = 'literal/path'
empty = []
owner.name = "galay"

[server]
enabled = false
ports = [8080, 8081]
)";

    assert(tomlEdge.parseString(tomlEdgeContent));
    assert(tomlEdge.getValue("title").value() == "value # not comment");
    assert(tomlEdge.getValue("path").value() == "literal/path");
    assert(tomlEdge.getValue("owner.name").value() == "galay");
    assert(tomlEdge.getValue("server.enabled").value() == "false");

    auto emptyArray = tomlEdge.getArray("empty");
    assert(emptyArray.empty());

    auto serverPorts = tomlEdge.getArray("server.ports");
    assert(serverPorts.size() == 2 && serverPorts[0] == "8080" && serverPorts[1] == "8081");

    TomlParser invalidToml;
    assert(!invalidToml.parseString("invalid line"));
    assert(!invalidToml.lastError().empty());
    assert(!invalidToml.parseString("bad = [1, 2"));
    assert(!invalidToml.parseString("[[products]]\nname = \"x\""));
    assert(!invalidToml.parseString("[]\nname = \"x\""));
    assert(!invalidToml.parseString("name = \"unterminated"));
    assert(!invalidToml.parseString("path = 'unterminated"));
    assert(!invalidToml.parseString("name = \"a\"\nname = \"b\""));
    assert(!invalidToml.parseString("[database]\nhost = \"a\"\n[database]\nhost = \"b\""));
    assert(!invalidToml.parseString("= \"value\""));
    assert(!invalidToml.parseString(".name = \"value\""));
    assert(!invalidToml.parseString("name. = \"value\""));
    assert(!invalidToml.parseString("[database.]\nhost = \"localhost\""));
    assert(!invalidToml.parseString("ports = [1,,2]"));
    assert(!invalidToml.parseString("names = [\"a]"));
    assert(!invalidToml.parseString("nested = [[1], [2]]"));
    assert(!invalidToml.parseString("inline = { name = \"galay\" }"));
    assert(!invalidToml.parseString("date = 2026-04-29T10:00:00Z"));
    assert(!invalidToml.parseString("enabled = True"));
    assert(!invalidToml.parseString("text = \"bad \\q escape\""));
    assert(!invalidToml.parseString("text = \"bad \\u12 escape\""));
    assert(!invalidToml.parseString("number = 01"));
    assert(!invalidToml.parseString("number = 1."));
    assert(!invalidToml.parseString("number = .1"));
    assert(!invalidToml.parseString("number = 1_000"));
    assert(!invalidToml.parseString("number = 1e10"));
    assert(!invalidToml.parseString("number = nan"));
    assert(!invalidToml.parseString("number = inf"));
    assert(!invalidToml.parseString("mixed = [1, \"a\"]"));
    assert(!invalidToml.parseString("trailing = [1, ,]"));
    assert(!invalidToml.parseString("database = \"x\"\n[database]\nhost = \"localhost\""));
    assert(!invalidToml.parseString("[database]\nhost = \"localhost\"\n[database.host]\nport = 1"));
    assert(!invalidToml.parseString("a = 1\na.b = 2"));
    assert(!invalidToml.parseString("a.b = 2\na = 1"));
    assert(!invalidToml.parseString("[db]\nhost = \"a\"\n[db]\nport = 1"));
    assert(!invalidToml.parseString("key = # missing"));
    assert(!invalidToml.parseString("\"quoted key\" = 1"));
    assert(!invalidToml.parseString("中文 = 1"));
    assert(!invalidToml.parseString("name = \"a\" \"b\""));
    assert(!invalidToml.parseString("name = 'a' 'b'"));
    assert(!invalidToml.parseString(R"(name = "unterminated by escaped quote\")"));
    assert(!invalidToml.parseString("ports = [\n    8000,\n"));
    assert(!invalidToml.parseString("ports = [\n    8000\n] trailing"));
    assert(!invalidToml.parseString("ports = [\n    8000\n[database]\nhost = \"localhost\"\n"));
    assert(!invalidToml.parseString("ports = [\n    8000\nname = \"x\"\n]"));
    assert(!invalidToml.parseString("names = [\n    \"a]\n]"));
    assert(!invalidToml.parseString("nested = [\n    [1]\n]"));
    assert(!invalidToml.parseString("tags = [\n    \"a\"\n    \"b\"\n]"));
    assert(!invalidToml.parseString("tags = [\n    'a'\n    'b'\n]"));

    TomlParser tomlCrlf;
    assert(tomlCrlf.parseString("name = \"galay\"\r\n[server]\r\nport = 8080\r\n"));
    assert(tomlCrlf.getValue("server.port").value() == "8080");

    std::cout << "Parser tests passed!" << std::endl;
}

// ==================== App (Args) Tests ====================

void testApp() {
    std::cout << "=== Testing App ===" << std::endl;

    App app("test-app", "Test application");

    app.addArg(Arg("name", "User name").shortName('n').required());
    app.addArg(Arg("count", "Count").shortName('c').type(ArgType::Int).defaultValue(1));
    app.addArg(Arg("verbose", "Verbose mode").shortName('v').flag());

    bool callbackCalled = false;
    app.callback([&callbackCalled](galay::utils::Cmd& cmd) {
        callbackCalled = true;
        assert(cmd.getAs<std::string>("name") == "John");
        assert(cmd.getAs<int>("count") == 5);
        assert(cmd.getAs<bool>("verbose") == true);
        return 0;
    });

    char* argv[] = {const_cast<char*>("test-app"),
                   const_cast<char*>("--name"),
                   const_cast<char*>("John"),
                   const_cast<char*>("-c"),
                   const_cast<char*>("5"),
                   const_cast<char*>("-v")};
    int result = app.run(6, argv);

    assert(result == 0);
    assert(callbackCalled);

    std::cout << "App tests passed!" << std::endl;
}

// ==================== Process Tests ====================

int main() {
    std::cout << "\n=== app_test ===" << std::endl;
    try {
        testParser();
        testApp();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
