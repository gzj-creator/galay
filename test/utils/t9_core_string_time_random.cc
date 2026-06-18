#include "test_common.hpp"

#include <limits>

void test_string() {
    std::cout << "=== Testing String ===" << std::endl;

    // Split
    auto parts = StringUtils::split("a,b,c", ',');
    assert(parts.size() == 3);
    assert(parts[0] == "a" && parts[1] == "b" && parts[2] == "c");

    // Join
    assert(StringUtils::join(parts, "-") == "a-b-c");

    // Trim
    assert(StringUtils::trim("  hello  ") == "hello");
    assert(StringUtils::trimLeft("  hello") == "hello");
    assert(StringUtils::trimRight("hello  ") == "hello");

    // Case conversion
    assert(StringUtils::toLower("HELLO") == "hello");
    assert(StringUtils::toUpper("hello") == "HELLO");

    // StartsWith/EndsWith
    assert(StringUtils::startsWith("hello world", "hello"));
    assert(StringUtils::endsWith("hello world", "world"));
    assert(StringUtils::contains("hello world", "lo wo"));

    // Replace
    assert(StringUtils::replace("aaa", "a", "b") == "bbb");
    assert(StringUtils::replaceFirst("aaa", "a", "b") == "baa");

    // Count
    assert(StringUtils::count("hello", 'l') == 2);
    assert(StringUtils::count("ababa", "ab") == 2);

    // Hex conversion
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    assert(StringUtils::toHex(data, 4, true) == "DEADBEEF");
    auto bytes = StringUtils::fromHex("DEADBEEF");
    assert(bytes.size() == 4 && bytes[0] == 0xDE);

    // Validation
    assert(StringUtils::isInteger("123"));
    assert(StringUtils::isInteger("-456"));
    assert(!StringUtils::isInteger("12.3"));
    assert(StringUtils::isFloat("3.14"));
    assert(StringUtils::isFloat("1e10"));
    assert(StringUtils::isBlank("   "));
    assert(!StringUtils::isBlank("  a  "));

    // Format
    assert(StringUtils::format("Hello %s, %d", "World", 42) == "Hello World, 42");

    // Parse
    assert(StringUtils::parse<int>("42") == 42);
    assert(StringUtils::parse<double>("3.14") == 3.14);

    // Edge cases for split
    auto emptySplit = StringUtils::split("", ',');
    assert(emptySplit.empty());

    auto singleChar = StringUtils::split("a", ',');
    assert(singleChar.size() == 1 && singleChar[0] == "a");

    auto onlyDelimiter = StringUtils::split(",", ',');
    assert(onlyDelimiter.size() == 2 && onlyDelimiter[0].empty() && onlyDelimiter[1].empty());

    auto endsWithDelimiter = StringUtils::split("a,", ',');
    assert(endsWithDelimiter.size() == 2 && endsWithDelimiter[0] == "a" && endsWithDelimiter[1].empty());

    auto startsWithDelimiter = StringUtils::split(",a", ',');
    assert(startsWithDelimiter.size() == 2 && startsWithDelimiter[0].empty() && startsWithDelimiter[1] == "a");

    auto multipleDelimiters = StringUtils::split("a,,b", ',');
    assert(multipleDelimiters.size() == 3 && multipleDelimiters[0] == "a" && multipleDelimiters[1].empty() && multipleDelimiters[2] == "b");

    auto emptyStringDelimiter = StringUtils::split("abc", "");
    assert(emptyStringDelimiter.size() == 1 && emptyStringDelimiter[0] == "abc");

    auto repeatedStringDelimiter = StringUtils::split("aaaa", "aa");
    assert(repeatedStringDelimiter.size() == 3);
    assert(repeatedStringDelimiter[0].empty() && repeatedStringDelimiter[1].empty() && repeatedStringDelimiter[2].empty());

    auto quoted = StringUtils::splitRespectQuotes("a,\"b,c\",d", ',');
    assert(quoted.size() == 3 && quoted[0] == "a" && quoted[1] == "\"b,c\"" && quoted[2] == "d");

    auto mismatchedQuote = StringUtils::splitRespectQuotes("a,\"b,c", ',');
    assert(mismatchedQuote.size() == 2 && mismatchedQuote[0] == "a" && mismatchedQuote[1] == "\"b,c");

    // Edge cases for string operations
    assert(StringUtils::trim("").empty());
    assert(StringUtils::toLower("").empty());
    assert(StringUtils::toUpper("").empty());
    assert(StringUtils::replace("", "a", "b").empty());
    assert(StringUtils::replaceFirst("", "a", "b").empty());
    assert(StringUtils::count("", 'a') == 0);

    // Edge cases for hex conversion
    assert(StringUtils::toHex(nullptr, 0).empty());
    assert(StringUtils::toHex(nullptr, 1).empty());
    assert(StringUtils::toVisibleHex(nullptr, 1).empty());
    assert(StringUtils::fromHex("").empty());
    assert(StringUtils::fromHex("invalid").empty());
    assert(StringUtils::fromHex("0").empty());
    assert(StringUtils::fromHex("DEADZEEF").empty());
    auto mixedCaseHex = StringUtils::fromHex("deAd");
    assert(mixedCaseHex.size() == 2 && mixedCaseHex[0] == 0xDE && mixedCaseHex[1] == 0xAD);

    // Edge cases for validation
    assert(!StringUtils::isInteger(""));
    assert(StringUtils::isInteger("+123"));
    assert(!StringUtils::isInteger("+"));
    assert(!StringUtils::isInteger("1e3"));
    assert(!StringUtils::isFloat(""));
    assert(StringUtils::isFloat(".5"));
    assert(StringUtils::isFloat("1."));
    assert(!StringUtils::isFloat("."));
    assert(!StringUtils::isFloat("1e"));
    assert(!StringUtils::isFloat("1e+"));
    assert(StringUtils::isBlank(""));
    assert(StringUtils::isBlank("   "));
    assert(StringUtils::isBlank("\t\n"));

    // Edge cases for formatting and parsing
    assert(StringUtils::format(nullptr).empty());
    assert(StringUtils::parse<int>("42abc", -1) == -1);
    assert(StringUtils::parse<int>(" 42 ", -1) == 42);
    assert(StringUtils::parse<int>("999999999999999999999", -7) == -7);
    assert(StringUtils::parse<double>("nan", -1.0) != -1.0);
    assert(StringUtils::parse<double>("1.5x", -1.0) == -1.0);

    std::cout << "String tests passed!" << std::endl;
}

// ==================== Random Tests ====================

void test_random() {
    std::cout << "=== Testing Random ===" << std::endl;

    auto& rng = Randomizer::instance();

    // Integer range
    for (int i = 0; i < 100; ++i) {
        int val = rng.randomInt(10, 20);
        assert(val >= 10 && val <= 20);
    }

    // Double range
    for (int i = 0; i < 100; ++i) {
        double val = rng.randomDouble(0.0, 1.0);
        assert(val >= 0.0 && val < 1.0);
    }

    // Random string
    std::string str = rng.randomString(10);
    assert(str.length() == 10);

    // Random hex
    std::string hex = rng.randomHex(8);
    assert(hex.length() == 8);

    // UUID
    std::string uuid = rng.uuid();
    assert(uuid.length() == 36);
    assert(uuid[8] == '-' && uuid[13] == '-' && uuid[18] == '-' && uuid[23] == '-');
    assert(uuid[14] == '4'); // Version 4
    assert(uuid[19] == '8' || uuid[19] == '9' || uuid[19] == 'a' || uuid[19] == 'b');
    for (size_t i = 0; i < uuid.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            continue;
        }
        assert(std::isxdigit(static_cast<unsigned char>(uuid[i])));
    }

    // Random bytes
    uint8_t buffer[16];
    rng.randomBytes(buffer, 16);

    // Edge cases
    // Same min/max should return min
    assert(rng.randomInt(5, 5) == 5);
    assert(rng.randomUint32(10, 10) == 10);
    assert(rng.randomUint64(20, 20) == 20);
    assert(rng.randomDouble(1.5, 1.5) == 1.5);
    assert(rng.randomFloat(2.5f, 2.5f) == 2.5f);

    // Empty/zero length strings
    assert(rng.randomString(0).empty());
    assert(rng.randomHex(0).empty());
    rng.randomBytes(nullptr, 0); // Should not crash
    rng.randomBytes(nullptr, 1); // Should not crash

    // Empty charset
    assert(rng.randomString(5, "").empty());

    // Invalid ranges and probability bounds
    assert(rng.randomInt(8, 3) == 8);
    assert(rng.randomUint32(8, 3) == 8);
    assert(rng.randomUint64(8, 3) == 8);
    assert(rng.randomDouble(8.0, 3.0) == 8.0);
    assert(rng.randomFloat(8.0f, 3.0f) == 8.0f);
    assert(!rng.randomBool(-0.5));
    assert(rng.randomBool(1.5));

    // Deterministic global seeding
    rng.seed(12345);
    int firstInt = rng.randomInt(1, 1000000);
    std::string firstHex = rng.randomHex(16);
    rng.seed(12345);
    assert(firstInt == rng.randomInt(1, 1000000));
    assert(firstHex == rng.randomHex(16));
    rng.reseed();

    // Local generator has no shared state and no internal mutex.
    RandomGenerator localA(20260606);
    RandomGenerator localB(20260606);
    assert(localA.randomUint64(1, std::numeric_limits<uint64_t>::max()) ==
           localB.randomUint64(1, std::numeric_limits<uint64_t>::max()));
    assert(localA.randomString(12, "ab").size() == 12);
    localA.randomBytes(nullptr, 4);

    std::cout << "Random tests passed!" << std::endl;
}

// ==================== System Tests ====================

void test_time_utilities() {
    std::cout << "=== Testing Time Utilities ===" << std::endl;

    const int64_t ms = Time::currentTimeMs();
    const int64_t us = Time::currentTimeUs();
    const int64_t ns = Time::currentTimeNs();
    assert(ms > 0);
    assert(us >= ms * 1000);
    assert(ns >= us * 1000);

    std::string gmt = Time::currentGMTTime();
    assert(!gmt.empty());

    std::string local = Time::currentLocalTime();
    assert(!local.empty());
    assert(Time::formatTime(0, "%Y", true) == "1970");
    assert(Time::formatTime(0, "%Y-%m-%d %H:%M:%S", true) == "1970-01-01 00:00:00");
    assert(Time::formatTime(0, "", true).empty());
    const std::string oversizedFormat(300, 'Y');
    assert(Time::formatTime(0, oversizedFormat.c_str(), true).empty());
    assert(Time::formatTime(0, nullptr, true).empty());
    assert(Time::currentGMTTime(nullptr).empty());
    assert(Time::currentLocalTime(nullptr).empty());
    std::cout << "  Current time: " << local << std::endl;

    ManualClock::reset();
    StopWatch<ManualClock> watch;
    ManualClock::advance(ManualClock::duration{25});
    assert(watch.elapsed() == ManualClock::duration{25});
    assert(watch.elapsedMs() == 25.0);

    watch.reset();
    assert(watch.startTime() == ManualClock::now());
    ManualClock::advance(ManualClock::duration{7});
    assert(watch.elapsed() == ManualClock::duration{7});

    StopWatch<ManualClock> explicitWatch(ManualClock::time_point{ManualClock::duration{2}});
    ManualClock::current = ManualClock::time_point{ManualClock::duration{9}};
    assert(explicitWatch.elapsed() == ManualClock::duration{7});

    ManualClock::reset();
    auto deadline = Deadline<ManualClock>::fromNow(ManualClock::duration{10});
    assert(deadline.timePoint() == ManualClock::now() + ManualClock::duration{10});
    assert(!deadline.expired());
    assert(deadline.remaining() == ManualClock::duration{10});
    ManualClock::advance(ManualClock::duration{11});
    assert(deadline.expired());
    assert(deadline.remaining() == ManualClock::duration{0});

    auto zeroDeadline = Deadline<ManualClock>::fromNow(ManualClock::duration{0});
    assert(zeroDeadline.expired());

    auto negativeDeadline = Deadline<ManualClock>::fromNow(ManualClock::duration{-1});
    assert(negativeDeadline.expired());

    using namespace std::chrono_literals;

    auto fixed = Backoff::fixed(5ms);
    assert(fixed.strategy() == Backoff::Strategy::Fixed);
    assert(fixed.next() == 5ms);
    assert(fixed.next() == 5ms);
    assert(fixed.attempts() == 2);
    fixed.reset();
    assert(fixed.attempts() == 0);
    assert(fixed.next() == 5ms);

    auto linear = Backoff::linear(1ms, 2ms, 5ms);
    assert(linear.next() == 1ms);
    assert(linear.next() == 3ms);
    assert(linear.next() == 5ms);
    assert(linear.next() == 5ms);

    auto cappedInitial = Backoff::linear(10ms, 1ms, 3ms);
    assert(cappedInitial.next() == 3ms);

    auto negativeLinear = Backoff::linear(-1ms, -2ms, 5ms);
    assert(negativeLinear.next() == 0ms);
    assert(negativeLinear.next() == 0ms);

    auto exponential = Backoff::exponential(1ms, 2.0, 10ms);
    assert(exponential.strategy() == Backoff::Strategy::Exponential);
    assert(exponential.next() == 1ms);
    assert(exponential.next() == 2ms);
    assert(exponential.next() == 4ms);
    assert(exponential.next() == 8ms);
    assert(exponential.next() == 10ms);

    auto flatExponential = Backoff::exponential(2ms, 0.5, 10ms);
    assert(flatExponential.next() == 2ms);
    assert(flatExponential.next() == 2ms);

    auto zeroMax = Backoff::exponential(2ms, 2.0, 0ms);
    assert(zeroMax.next() == 0ms);

    auto zero = Backoff::fixed(-1ms);
    assert(zero.next() == 0ms);

    std::cout << "Time utility tests passed!" << std::endl;
}

// ==================== ByteQueueView Tests ====================

void test_type_name() {
    std::cout << "=== Testing TypeName ===" << std::endl;

    std::string intName = getTypeName<int>();
    assert(intName == "int");
    assert(getTypeName(0) == intName);

    std::string strName = getTypeName<std::string>();
    assert(strName.find("string") != std::string::npos);

    std::vector<int> vec;
    std::string vecName = getTypeName(vec);
    assert(vecName.find("vector") != std::string::npos);
    assert(getTypeName<std::vector<int>>() == vecName);
    assert(demangleSymbol(nullptr).empty());

    std::cout << "  int type name: " << intName << std::endl;
    std::cout << "  string type name: " << strName << std::endl;

    std::cout << "TypeName tests passed!" << std::endl;
}

// ==================== Base64 Tests ====================

int main() {
    std::cout << "\n=== core_test ===" << std::endl;
    try {
        test_string();
        test_random();
        test_time_utilities();
        test_type_name();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
