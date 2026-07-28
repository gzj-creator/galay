#include "test_common.hpp"

#include <sstream>

void test_parser() {
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

void test_app_basic() {
    App app("test-app", "Test application");

    int boundCount = 0;
    auto& name = app.opt<std::string>("name", 'n', "User name").required();
    auto& count = app.opt<int>("count", 'c', "Count").def(1).bind(&boundCount);
    auto& verbose = app.flag("verbose", 'v', "Verbose mode");
    auto& ratio = app.opt<double>("ratio", "Ratio").def(0.5);

    bool callbackCalled = false;
    app.on([&](galay::utils::Cmd&) {
        callbackCalled = true;
        return 0;
    });

    const char* argv[] = {"test-app", "--name", "John", "-c", "5", "-v", "--ratio=0.25"};
    assert(app.run(7, argv) == 0);
    assert(callbackCalled);
    assert(name.value() == "John");
    assert(count.value() == 5 && boundCount == 5);
    assert(verbose.value());
    assert(ratio.value() == 0.25);
    assert(app.has("name") && !app.has("nope"));
}

void test_app_defaults_and_errors() {
    std::ostringstream out;
    std::ostringstream err;

    App app("test-app");
    auto& count = app.opt<int>("count", 'c', "Count").def(7);
    auto& required = app.opt<std::string>("who", 'w', "Who").required();

    const char* missing[] = {"test-app"};
    assert(app.run(1, missing, out, err) == 1);
    assert(err.str().find("missing required argument: --who") != std::string::npos);
    assert(!required.isSet());
    assert(count.value() == 7);

    err.str({});
    const char* badValue[] = {"test-app", "-w", "x", "-c", "12abc"};
    assert(app.run(5, badValue, out, err) == 1);
    assert(err.str().find("invalid value: -c") != std::string::npos);

    err.str({});
    const char* unknown[] = {"test-app", "-w", "x", "--nope"};
    assert(app.run(4, unknown, out, err) == 1);
    assert(err.str().find("unknown option: --nope") != std::string::npos);

    err.str({});
    const char* noValue[] = {"test-app", "-w"};
    assert(app.run(2, noValue, out, err) == 1);
    assert(err.str().find("missing value: -w") != std::string::npos);

    // 重复解析不残留上一次状态
    const char* good[] = {"test-app", "-w", "y"};
    assert(app.run(3, good, out, err) == 0);
    assert(required.value() == "y" && count.value() == 7);
}

void test_app_flags_and_multi() {
    App app("test-app");
    auto& color = app.flag("color", "Colored output").def(true);
    auto& quiet = app.flag("quiet", 'q', "Quiet");
    auto& tags = app.opt<std::string>("tag", 't', "Tags").multi();

    std::vector<std::string> boundTags;
    tags.bindAll(&boundTags);

    const char* argv[] = {"test-app", "--no-color", "-q", "-t", "a", "--tag=b", "-tc"};
    assert(app.run(7, argv) == 0);
    assert(!color.value());
    assert(quiet.value());
    assert(tags.values().size() == 3);
    assert(tags.values()[0] == "a" && tags.values()[2] == "c");
    assert(tags.value() == "c");
    assert(boundTags == tags.values());
}

void test_app_positional_and_choices() {
    App app("test-app");
    auto& mode = app.opt<std::string>("mode", 'm', "Mode").choices({"fast", "slow"}).def("fast");
    auto& input = app.pos<std::string>("input", "Input file").required();
    auto& extras = app.pos<int>("extra", "Extra numbers").many();

    const char* argv[] = {"test-app", "-m", "slow", "in.txt", "1", "2", "3"};
    assert(app.run(7, argv) == 0);
    assert(mode.value() == "slow");
    assert(input.value() == "in.txt");
    assert(extras.values().size() == 3 && extras.values()[2] == 3);
    assert(app.rest().empty());

    std::ostringstream out;
    std::ostringstream err;
    const char* badChoice[] = {"test-app", "-m", "medium", "in.txt"};
    assert(app.run(4, badChoice, out, err) == 1);
    assert(err.str().find("value not allowed: -m") != std::string::npos);

    err.str({});
    const char* missingPositional[] = {"test-app"};
    assert(app.run(1, missingPositional, out, err) == 1);
    assert(err.str().find("missing required argument: input") != std::string::npos);
}

void test_app_end_of_options() {
    App app("test-app");
    auto& flag = app.flag("verbose", 'v', "Verbose");
    auto& rest = app.pos<std::string>("args", "Passthrough").many();

    const char* argv[] = {"test-app", "-v", "--", "-v", "--nope", "plain"};
    assert(app.run(6, argv) == 0);
    assert(flag.value());
    assert(rest.values().size() == 3);
    assert(rest.values()[0] == "-v" && rest.values()[1] == "--nope");
}

void test_app_subcommands() {
    App app("git-like", "Subcommand test");
    app.version("2.0.0");

    auto& clone = app.sub("clone", "Clone a repo");
    auto& depth = clone.opt<int>("depth", 'd', "Depth").def(0);
    auto& url = clone.pos<std::string>("url", "Repo url").required();

    auto& remote = app.sub("remote", "Remote ops");
    auto& add = remote.sub("add", "Add remote");
    auto& remoteName = add.pos<std::string>("name", "Remote name").required();

    int cloneHits = 0;
    int addHits = 0;
    clone.on([&](galay::utils::Cmd&) { ++cloneHits; return 3; });
    add.on([&](galay::utils::Cmd&) { ++addHits; return 4; });

    const char* cloneArgv[] = {"git-like", "clone", "-d", "1", "http://repo"};
    assert(app.run(5, cloneArgv) == 3);
    assert(cloneHits == 1 && addHits == 0);
    assert(depth.value() == 1 && url.value() == "http://repo");

    const char* addArgv[] = {"git-like", "remote", "add", "origin"};
    assert(app.run(4, addArgv) == 4);
    assert(addHits == 1 && remoteName.value() == "origin");
    assert(app.selected().name() == "add");

    std::ostringstream out;
    std::ostringstream err;
    const char* unknownSub[] = {"git-like", "nosuch"};
    assert(app.run(2, unknownSub, out, err) == 1);
    assert(err.str().find("unknown subcommand: nosuch") != std::string::npos);
}

void test_app_help_and_version() {
    App app("helper", "Helper description");
    app.version("1.2.3");
    app.opt<int>("port", 'p', "Listen port").def(8080);
    app.flag("verbose", 'v', "Verbose");
    app.opt<std::string>("mode", "Mode").choices({"a", "b"});
    app.pos<std::string>("file", "Input file").required();
    app.sub("run", "Run it");

    std::ostringstream out;
    std::ostringstream err;
    const char* helpArgv[] = {"helper", "--help"};
    assert(app.run(2, helpArgv, out, err) == 0);

    const std::string help = out.str();
    assert(help.find("Usage: helper <command> [options] <file>") != std::string::npos);
    assert(help.find("Helper description") != std::string::npos);
    assert(help.find("Commands:") != std::string::npos);
    assert(help.find("Arguments:") != std::string::npos);
    assert(help.find("-p, --port <int>") != std::string::npos);
    assert(help.find("(default: 8080)") != std::string::npos);
    assert(help.find("{a|b}") != std::string::npos);
    assert(help.find("[required]") != std::string::npos);
    assert(help.find("--help") != std::string::npos);
    // 帮助顺序必须与声明顺序一致
    assert(help.find("--port") < help.find("--verbose"));
    assert(help.find("--verbose") < help.find("--mode"));

    out.str({});
    const char* versionArgv[] = {"helper", "--version"};
    assert(app.run(2, versionArgv, out, err) == 0);
    assert(out.str() == "1.2.3\n");
}

void test_app_edge_cases() {
    // 子命令内出错时帮助应来自子命令
    {
        App app("git");
        auto& clone = app.sub("clone", "Clone repo");
        clone.opt<int>("depth", 'd', "Depth").def(0);
        std::ostringstream out;
        std::ostringstream err;
        const char* argv[] = {"git", "clone", "--nope"};
        assert(app.run(3, argv, out, err) == 1);
        assert(err.str().find("Usage: clone") != std::string::npos);
    }
    // 未声明版本时 --version 属于未知选项
    {
        App app("tool");
        std::ostringstream out;
        std::ostringstream err;
        const char* argv[] = {"tool", "--version"};
        assert(app.run(2, argv, out, err) == 1);
        assert(err.str().find("unknown option: --version") != std::string::npos);
    }
    // 用户占用 -h 时不再抢占为帮助
    {
        App app("tool");
        auto& host = app.opt<std::string>("host", 'h', "Host");
        const char* argv[] = {"tool", "-h", "localhost"};
        assert(app.run(3, argv) == 0);
        assert(host.value() == "localhost");
    }
    // 单个 "-" 按位置参数处理，负数取值不被误判为选项
    {
        App app("tool");
        auto& file = app.pos<std::string>("file", "File");
        auto& offset = app.opt<int>("offset", 'o', "Offset").def(0);
        const char* argv[] = {"tool", "--offset=-5", "-"};
        assert(app.run(3, argv) == 0);
        assert(file.value() == "-" && offset.value() == -5);
    }
    // 未被命名位置参数消费的剩余参数进 rest()
    {
        App app("tool");
        const char* argv[] = {"tool", "a", "b"};
        assert(app.run(3, argv) == 0);
        assert(app.rest().size() == 2 && app.rest()[1] == "b");
    }
    // 根命令带 many() 位置参数时，子命令仍可被识别
    {
        App app("tool");
        auto& files = app.pos<std::string>("files", "Files").many();
        auto& sub = app.sub("build", "Build");
        int hits = 0;
        sub.on([&](galay::utils::Cmd&) { ++hits; return 0; });

        const char* argv[] = {"tool", "build"};
        assert(app.run(2, argv) == 0);
        assert(hits == 1 && files.values().empty());

        const char* plain[] = {"tool", "a.txt", "b.txt"};
        assert(app.run(3, plain) == 0);
        assert(hits == 1 && files.values().size() == 2);
    }
    // 子命令 --help 不应先触发父命令的必选校验
    {
        App app("tool");
        app.opt<std::string>("name", 'n', "Name").required();
        auto& sub = app.sub("serve", "Serve");
        sub.opt<int>("port", 'p', "Port").def(80);
        std::ostringstream out;
        std::ostringstream err;
        const char* argv[] = {"tool", "serve", "--help"};
        assert(app.run(3, argv, out, err) == 0);
        assert(err.str().empty());
        assert(out.str().find("Usage: serve") != std::string::npos);
        assert(out.str().find("--port") != std::string::npos);
    }
    // 缺少必选参数时 --help 仍应正常输出
    {
        App app("tool");
        app.opt<std::string>("name", 'n', "Name").required();
        std::ostringstream out;
        std::ostringstream err;
        const char* argv[] = {"tool", "--help"};
        assert(app.run(2, argv, out, err) == 0);
        assert(err.str().empty());
        assert(out.str().find("Usage: tool") != std::string::npos);
    }
    // "--" 之后的 --help 属于普通取值，不触发帮助
    {
        App app("tool");
        auto& rest = app.pos<std::string>("rest", "Rest").many();
        std::ostringstream out;
        std::ostringstream err;
        const char* argv[] = {"tool", "--", "--help"};
        assert(app.run(3, argv, out, err) == 0);
        assert(out.str().empty());
        assert(rest.values().size() == 1 && rest.values()[0] == "--help");
    }
}

void test_app() {
    std::cout << "=== Testing App ===" << std::endl;

    test_app_basic();
    test_app_defaults_and_errors();
    test_app_flags_and_multi();
    test_app_positional_and_choices();
    test_app_end_of_options();
    test_app_subcommands();
    test_app_help_and_version();
    test_app_edge_cases();

    std::cout << "App tests passed!" << std::endl;
}

// ==================== Process Tests ====================

int main() {
    std::cout << "\n=== app_test ===" << std::endl;
    try {
        test_parser();
        test_app();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
