#include "test_common.hpp"

void testBase64() {
    std::cout << "=== Testing Base64 ===" << std::endl;

    // Basic encoding
    std::string input = "Hello, World!";
    std::string encoded = Base64Util::Base64Encode(input);
    assert(!encoded.empty());
    std::cout << "  Encoded: " << encoded << std::endl;

    // Basic decoding
    std::string decoded = Base64Util::Base64Decode(encoded);
    assert(decoded == input);
    std::cout << "  Decoded: " << decoded << std::endl;

    // Test with binary data
    unsigned char binaryData[] = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD, 0xFC};
    std::string binaryEncoded = Base64Util::Base64Encode(binaryData, sizeof(binaryData));
    assert(!binaryEncoded.empty());
    std::string binaryDecoded = Base64Util::Base64Decode(binaryEncoded);
    assert(binaryDecoded.size() == sizeof(binaryData));
    for (size_t i = 0; i < sizeof(binaryData); ++i) {
        assert(static_cast<unsigned char>(binaryDecoded[i]) == binaryData[i]);
    }

    // Test URL-safe encoding
    std::string urlEncoded = Base64Util::Base64Encode(input, true);
    assert(!urlEncoded.empty());
    std::string urlDecoded = Base64Util::Base64Decode(urlEncoded);
    assert(urlDecoded == input);

    // Test PEM format (64 chars per line)
    std::string longInput = "This is a longer string that will be encoded in PEM format with line breaks every 64 characters to test the line breaking functionality.";
    std::string pemEncoded = Base64Util::Base64EncodePem(longInput);
    assert(pemEncoded.find('\n') != std::string::npos); // Should contain line breaks
    std::string pemDecoded = Base64Util::Base64Decode(pemEncoded, true); // Remove line breaks
    assert(pemDecoded == longInput);

    // Test MIME format (76 chars per line)
    std::string mimeEncoded = Base64Util::Base64EncodeMime(longInput);
    assert(mimeEncoded.find('\n') != std::string::npos); // Should contain line breaks
    std::string mimeDecoded = Base64Util::Base64Decode(mimeEncoded, true);
    assert(mimeDecoded == longInput);

    // Edge cases
    // Empty string
    assert(Base64Util::Base64Encode("").empty());
    assert(Base64Util::Base64Decode("").empty());

    // Single character
    std::string singleChar = "A";
    std::string singleEncoded = Base64Util::Base64Encode(singleChar);
    assert(Base64Util::Base64Decode(singleEncoded) == singleChar);

    // Two characters
    std::string twoChars = "AB";
    std::string twoEncoded = Base64Util::Base64Encode(twoChars);
    assert(Base64Util::Base64Decode(twoEncoded) == twoChars);

    // Three characters (no padding needed)
    std::string threeChars = "ABC";
    std::string threeEncoded = Base64Util::Base64Encode(threeChars);
    assert(Base64Util::Base64Decode(threeEncoded) == threeChars);

    // Test all ASCII characters
    std::string allAscii;
    for (int i = 0; i < 128; ++i) {
        allAscii += static_cast<char>(i);
    }
    std::string asciiEncoded = Base64Util::Base64Encode(allAscii);
    std::string asciiDecoded = Base64Util::Base64Decode(asciiEncoded);
    assert(asciiDecoded == allAscii);

    // Test with special characters
    std::string special = "!@#$%^&*()_+-=[]{}|;':\",./<>?`~";
    std::string specialEncoded = Base64Util::Base64Encode(special);
    std::string specialDecoded = Base64Util::Base64Decode(specialEncoded);
    assert(specialDecoded == special);

    // Test with UTF-8 characters
    std::string utf8 = "Hello 世界 🌍";
    std::string utf8Encoded = Base64Util::Base64Encode(utf8);
    std::string utf8Decoded = Base64Util::Base64Decode(utf8Encoded);
    assert(utf8Decoded == utf8);

    // Test standard vs URL-safe encoding differences
    std::string testData = "\xFB\xFF"; // Will produce '+' and '/' in standard encoding
    std::string standardEncoded = Base64Util::Base64Encode(testData, false);
    std::string urlSafeEncoded = Base64Util::Base64Encode(testData, true);
    // Both should decode to the same value
    assert(Base64Util::Base64Decode(standardEncoded) == testData);
    assert(Base64Util::Base64Decode(urlSafeEncoded) == testData);

    // Test decoding with line breaks
    std::string encodedWithBreaks = "SGVsbG8s\nIFdvcmxk\nIQ==";
    std::string decodedWithBreaks = Base64Util::Base64Decode(encodedWithBreaks, true);
    assert(decodedWithBreaks == "Hello, World!");

    // Test invalid input handling
    bool exceptionThrown = false;
    try {
        Base64Util::Base64Decode("Invalid@#$%");
    } catch (const std::runtime_error&) {
        exceptionThrown = true;
    }
    assert(exceptionThrown);

    // Performance test with large data
    std::string largeData(10000, 'X');
    auto startEncode = std::chrono::high_resolution_clock::now();
    std::string largeEncoded = Base64Util::Base64Encode(largeData);
    auto endEncode = std::chrono::high_resolution_clock::now();
    auto encodeTime = std::chrono::duration_cast<std::chrono::microseconds>(endEncode - startEncode).count();

    auto startDecode = std::chrono::high_resolution_clock::now();
    std::string largeDecoded = Base64Util::Base64Decode(largeEncoded);
    auto endDecode = std::chrono::high_resolution_clock::now();
    auto decodeTime = std::chrono::duration_cast<std::chrono::microseconds>(endDecode - startDecode).count();

    assert(largeDecoded == largeData);
    std::cout << "  Performance (10KB): Encode=" << encodeTime << "μs, Decode=" << decodeTime << "μs" << std::endl;

#if __cplusplus >= 201703L
    // Test string_view overloads (C++17+)
    std::cout << "  Testing string_view interfaces..." << std::endl;

    std::string_view inputView = "Test string_view";
    std::string viewEncoded = Base64Util::Base64EncodeView(inputView);
    std::string viewDecoded = Base64Util::Base64DecodeView(viewEncoded);
    assert(viewDecoded == inputView);

    // Test URL-safe with string_view
    std::string viewUrlEncoded = Base64Util::Base64EncodeView(inputView, true);
    std::string viewUrlDecoded = Base64Util::Base64DecodeView(viewUrlEncoded);
    assert(viewUrlDecoded == inputView);

    // Test PEM with string_view
    std::string_view viewForPem = "Test PEM with string_view that is long enough to trigger line breaks in the output";
    std::string pemViewEncoded = Base64Util::Base64EncodePemView(viewForPem);
    assert(pemViewEncoded.find('\n') != std::string::npos);
    std::string pemViewDecoded = Base64Util::Base64DecodeView(pemViewEncoded, true);
    assert(pemViewDecoded == viewForPem);

    // Test MIME with string_view
    std::string_view viewForMime = "Test MIME with string_view that is long enough to trigger line breaks in the output";
    std::string mimeViewEncoded = Base64Util::Base64EncodeMimeView(viewForMime);
    assert(mimeViewEncoded.find('\n') != std::string::npos);
    std::string mimeViewDecoded = Base64Util::Base64DecodeView(mimeViewEncoded, true);
    assert(mimeViewDecoded == viewForMime);

    // Test with substring view
    std::string fullString = "Hello, World! This is a test.";
    std::string_view subView(fullString.data() + 7, 5); // "World"
    std::string subEncoded = Base64Util::Base64EncodeView(subView);
    std::string subDecoded = Base64Util::Base64DecodeView(subEncoded);
    assert(subDecoded == "World");

    std::cout << "  string_view tests passed!" << std::endl;
#endif

    std::cout << "Base64 tests passed!" << std::endl;
}

// ==================== MD5 Tests ====================

void testMD5() {
    std::cout << "=== Testing MD5 ===" << std::endl;

    // Test empty string
    std::string emptyHash = MD5Util::MD5("");
    assert(emptyHash == "d41d8cd98f00b204e9800998ecf8427e");
    std::cout << "  Empty string: " << emptyHash << std::endl;

    // Test simple string
    std::string simpleHash = MD5Util::MD5("hello");
    assert(simpleHash == "5d41402abc4b2a76b9719d911017c592");
    std::cout << "  'hello': " << simpleHash << std::endl;

    // Test standard test vectors
    assert(MD5Util::MD5("a") == "0cc175b9c0f1b6a831c399e269772661");
    assert(MD5Util::MD5("abc") == "900150983cd24fb0d6963f7d28e17f72");
    assert(MD5Util::MD5("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    assert(MD5Util::MD5("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    assert(MD5Util::MD5("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") == "d174ab98d277d9f5a5611c2c9f419d9f");
    assert(MD5Util::MD5("12345678901234567890123456789012345678901234567890123456789012345678901234567890") == "57edf4a22be3c955ac49da2e2107b67a");

    // Test with binary data
    unsigned char binaryData[] = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD, 0xFC};
    std::string binaryHash = MD5Util::MD5(binaryData, sizeof(binaryData));
    assert(!binaryHash.empty());
    assert(binaryHash.length() == 32); // MD5 is 128 bits = 32 hex chars
    std::cout << "  Binary data: " << binaryHash << std::endl;

    // Test MD5Raw (returns raw bytes)
    auto rawDigest = MD5Util::MD5Raw("hello");
    assert(rawDigest.size() == 16); // MD5 is 128 bits = 16 bytes

    // Verify raw digest matches hex string
    std::string hexFromRaw;
    for (auto byte : rawDigest) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", byte);
        hexFromRaw += buf;
    }
    assert(hexFromRaw == simpleHash);

    // Test with special characters
    std::string specialHash = MD5Util::MD5("!@#$%^&*()_+-=[]{}|;':\",./<>?`~");
    assert(!specialHash.empty());
    assert(specialHash.length() == 32);

    // Test with UTF-8
    std::string utf8Hash = MD5Util::MD5("Hello 世界 🌍");
    assert(!utf8Hash.empty());
    assert(utf8Hash.length() == 32);
    std::cout << "  UTF-8: " << utf8Hash << std::endl;

    // Test with long string
    std::string longString(10000, 'X');
    auto startHash = std::chrono::high_resolution_clock::now();
    std::string longHash = MD5Util::MD5(longString);
    auto endHash = std::chrono::high_resolution_clock::now();
    auto hashTime = std::chrono::duration_cast<std::chrono::microseconds>(endHash - startHash).count();

    assert(!longHash.empty());
    assert(longHash.length() == 32);
    std::cout << "  Performance (10KB): " << hashTime << "μs" << std::endl;

    // Test consistency - same input should always produce same hash
    for (int i = 0; i < 10; ++i) {
        assert(MD5Util::MD5("test") == "098f6bcd4621d373cade4e832627b4f6");
    }

    // Test different inputs produce different hashes
    std::string hash1 = MD5Util::MD5("test1");
    std::string hash2 = MD5Util::MD5("test2");
    assert(hash1 != hash2);

    // Test case sensitivity
    std::string lowerHash = MD5Util::MD5("hello");
    std::string upperHash = MD5Util::MD5("HELLO");
    assert(lowerHash != upperHash);

#if __cplusplus >= 201703L
    // Test string_view version
    std::cout << "  Testing string_view interface..." << std::endl;

    std::string_view view = "test string_view";
    std::string viewHash = MD5Util::MD5View(view);
    assert(!viewHash.empty());
    assert(viewHash.length() == 32);

    // Verify string_view produces same result as string
    std::string str = "test string_view";
    assert(MD5Util::MD5View(view) == MD5Util::MD5(str));

    // Test with substring view
    std::string fullString = "Hello, World! This is a test.";
    std::string_view subView(fullString.data() + 7, 5); // "World"
    std::string subHash = MD5Util::MD5View(subView);
    assert(subHash == MD5Util::MD5("World"));

    // Test MD5RawView
    auto rawViewDigest = MD5Util::MD5RawView(view);
    assert(rawViewDigest.size() == 16);

    std::cout << "  string_view tests passed!" << std::endl;
#endif

    // Test known MD5 collisions awareness (informational)
    // Note: MD5 is cryptographically broken, but still useful for checksums
    std::cout << "  Note: MD5 is not cryptographically secure" << std::endl;

    std::cout << "MD5 tests passed!" << std::endl;
}

// ==================== MurmurHash3 Tests ====================

void testMurmurHash3() {
    std::cout << "=== Testing MurmurHash3 ===" << std::endl;

    // Test 32-bit hash with empty string
    uint32_t emptyHash32 = MurmurHash3Util::Hash32("", 0);
    std::cout << "  Empty string (32-bit): 0x" << std::hex << emptyHash32 << std::dec << std::endl;
    assert(emptyHash32 == 0); // Empty string with seed 0 should give 0

    // Test 32-bit hash with simple strings
    uint32_t hash32_hello = MurmurHash3Util::Hash32("hello");
    std::cout << "  'hello' (32-bit): 0x" << std::hex << hash32_hello << std::dec << std::endl;
    assert(hash32_hello != 0);

    // Test with different seeds produce different hashes
    uint32_t hash_seed0 = MurmurHash3Util::Hash32("test", 0);
    uint32_t hash_seed1 = MurmurHash3Util::Hash32("test", 1);
    uint32_t hash_seed42 = MurmurHash3Util::Hash32("test", 42);
    assert(hash_seed0 != hash_seed1);
    assert(hash_seed0 != hash_seed42);
    assert(hash_seed1 != hash_seed42);

    // Test 128-bit hash
    std::string hash128_empty = MurmurHash3Util::Hash128("");
    assert(hash128_empty.length() == 32); // 128 bits = 32 hex chars
    std::cout << "  Empty string (128-bit): " << hash128_empty << std::endl;

    std::string hash128_hello = MurmurHash3Util::Hash128("hello");
    assert(hash128_hello.length() == 32);
    std::cout << "  'hello' (128-bit): " << hash128_hello << std::endl;

    // Test 128-bit raw output
    auto raw128 = MurmurHash3Util::Hash128Raw("test");
    assert(raw128.size() == 2); // Two 64-bit values
    std::cout << "  'test' (128-bit raw): [0x" << std::hex << raw128[0]
              << ", 0x" << raw128[1] << "]" << std::dec << std::endl;

    // Test consistency - same input should always produce same hash
    for (int i = 0; i < 10; ++i) {
        assert(MurmurHash3Util::Hash32("consistent") == MurmurHash3Util::Hash32("consistent"));
        assert(MurmurHash3Util::Hash128("consistent") == MurmurHash3Util::Hash128("consistent"));
    }

    // Test different inputs produce different hashes
    uint32_t hash1 = MurmurHash3Util::Hash32("test1");
    uint32_t hash2 = MurmurHash3Util::Hash32("test2");
    assert(hash1 != hash2);

    std::string hash128_1 = MurmurHash3Util::Hash128("test1");
    std::string hash128_2 = MurmurHash3Util::Hash128("test2");
    assert(hash128_1 != hash128_2);

    // Test case sensitivity
    uint32_t lowerHash = MurmurHash3Util::Hash32("hello");
    uint32_t upperHash = MurmurHash3Util::Hash32("HELLO");
    assert(lowerHash != upperHash);

    // Test with binary data
    unsigned char binaryData[] = {0x00, 0x01, 0x02, 0x03, 0xFF, 0xFE, 0xFD, 0xFC};
    uint32_t binaryHash32 = MurmurHash3Util::Hash32(binaryData, sizeof(binaryData));
    assert(binaryHash32 != 0);

    std::string binaryHash128 = MurmurHash3Util::Hash128(binaryData, sizeof(binaryData));
    assert(binaryHash128.length() == 32);

    // Test with various lengths to cover all tail cases
    for (size_t len = 0; len <= 20; ++len) {
        std::string testStr(len, 'X');
        uint32_t h32 = MurmurHash3Util::Hash32(testStr);
        std::string h128 = MurmurHash3Util::Hash128(testStr);
        assert(h128.length() == 32);
        // Just verify it doesn't crash and produces output
    }

    // Test with special characters
    uint32_t specialHash = MurmurHash3Util::Hash32("!@#$%^&*()_+-=[]{}|;':\",./<>?`~");
    assert(specialHash != 0);

    // Test with UTF-8
    uint32_t utf8Hash = MurmurHash3Util::Hash32("Hello 世界 🌍");
    assert(utf8Hash != 0);
    std::cout << "  UTF-8 (32-bit): 0x" << std::hex << utf8Hash << std::dec << std::endl;

    // Performance test with large data
    std::string largeData(10000, 'X');

    auto start32 = std::chrono::high_resolution_clock::now();
    uint32_t largeHash32 = MurmurHash3Util::Hash32(largeData);
    auto end32 = std::chrono::high_resolution_clock::now();
    auto time32 = std::chrono::duration_cast<std::chrono::microseconds>(end32 - start32).count();

    auto start128 = std::chrono::high_resolution_clock::now();
    std::string largeHash128 = MurmurHash3Util::Hash128(largeData);
    auto end128 = std::chrono::high_resolution_clock::now();
    auto time128 = std::chrono::duration_cast<std::chrono::microseconds>(end128 - start128).count();

    assert(largeHash32 != 0);
    assert(largeHash128.length() == 32);
    std::cout << "  Performance (10KB): Hash32=" << time32 << "μs, Hash128=" << time128 << "μs" << std::endl;

    // Test avalanche effect - small change in input should cause large change in output
    uint32_t hashA = MurmurHash3Util::Hash32("test");
    uint32_t hashB = MurmurHash3Util::Hash32("tess"); // Changed one character

    // Count different bits
    uint32_t diff = hashA ^ hashB;
    int bitsDifferent = 0;
    for (int i = 0; i < 32; ++i) {
        if (diff & (1U << i)) bitsDifferent++;
    }

    // Good hash should have roughly 50% bits different (avalanche effect)
    assert(bitsDifferent > 10); // At least some bits should differ
    std::cout << "  Avalanche effect: " << bitsDifferent << "/32 bits differ" << std::endl;

#if __cplusplus >= 201703L
    // Test string_view versions
    std::cout << "  Testing string_view interfaces..." << std::endl;

    std::string_view view = "test string_view";
    uint32_t viewHash32 = MurmurHash3Util::Hash32View(view);
    std::string viewHash128 = MurmurHash3Util::Hash128View(view);
    auto viewHash128Raw = MurmurHash3Util::Hash128RawView(view);

    assert(viewHash32 != 0);
    assert(viewHash128.length() == 32);
    assert(viewHash128Raw.size() == 2);

    // Verify string_view produces same result as string
    std::string str = "test string_view";
    assert(MurmurHash3Util::Hash32View(view) == MurmurHash3Util::Hash32(str));
    assert(MurmurHash3Util::Hash128View(view) == MurmurHash3Util::Hash128(str));

    // Test with substring view
    std::string fullString = "Hello, World! This is a test.";
    std::string_view subView(fullString.data() + 7, 5); // "World"
    uint32_t subHash = MurmurHash3Util::Hash32View(subView);
    assert(subHash == MurmurHash3Util::Hash32("World"));

    std::cout << "  string_view tests passed!" << std::endl;
#endif

    // Test distribution quality (basic check)
    std::cout << "  Testing hash distribution..." << std::endl;
    const int numBuckets = 100;
    std::vector<int> buckets(numBuckets, 0);

    for (int i = 0; i < 10000; ++i) {
        std::string key = "key" + std::to_string(i);
        uint32_t hash = MurmurHash3Util::Hash32(key);
        buckets[hash % numBuckets]++;
    }

    // Check that distribution is reasonably uniform
    // Each bucket should have roughly 100 items (10000/100)
    int minCount = *std::min_element(buckets.begin(), buckets.end());
    int maxCount = *std::max_element(buckets.begin(), buckets.end());

    // Allow some variance but not too much
    assert(minCount > 50);  // Not too few
    assert(maxCount < 150); // Not too many
    std::cout << "  Distribution: min=" << minCount << ", max=" << maxCount << " (expected ~100)" << std::endl;

    std::cout << "MurmurHash3 tests passed!" << std::endl;
}

// ==================== Salt Generator Tests ====================

void testSaltGenerator() {
    std::cout << "=== Testing Salt Generator ===" << std::endl;

    // Test hex salt generation
    std::string hexSalt = SaltGenerator::generateHex(16);
    assert(hexSalt.length() == 32); // 16 bytes = 32 hex chars
    assert(SaltGenerator::isValidHex(hexSalt));
    std::cout << "  Hex salt (16 bytes): " << hexSalt << std::endl;

    // Test base64 salt generation
    std::string base64Salt = SaltGenerator::generateBase64(16);
    assert(!base64Salt.empty());
    assert(SaltGenerator::isValidBase64(base64Salt));
    std::cout << "  Base64 salt (16 bytes): " << base64Salt << std::endl;

    // Test raw bytes generation
    auto bytesSalt = SaltGenerator::generateBytes(32);
    assert(bytesSalt.size() == 32);
    std::cout << "  Raw bytes salt: " << bytesSalt.size() << " bytes generated" << std::endl;

    // Test secure hex generation
    std::string secureHex = SaltGenerator::generateSecureHex(32);
    assert(secureHex.length() == 64); // 32 bytes = 64 hex chars
    assert(SaltGenerator::isValidHex(secureHex));
    std::cout << "  Secure hex salt (32 bytes): " << secureHex.substr(0, 32) << "..." << std::endl;

    // Test secure base64 generation
    std::string secureBase64 = SaltGenerator::generateSecureBase64(24);
    assert(!secureBase64.empty());
    assert(SaltGenerator::isValidBase64(secureBase64));
    std::cout << "  Secure base64 salt: " << secureBase64 << std::endl;

    // Test secure bytes generation
    auto secureBytes = SaltGenerator::generateSecureBytes(16);
    assert(secureBytes.size() == 16);

    // Test bcrypt salt generation
    std::string bcryptSalt = SaltGenerator::generateBcryptSalt();
    assert(bcryptSalt.length() == 22);
    std::cout << "  Bcrypt salt (22 chars): " << bcryptSalt << std::endl;

    // Test custom charset generation
    std::string customSalt = SaltGenerator::generateCustom(20, "0123456789");
    assert(customSalt.length() == 20);
    for (char c : customSalt) {
        assert(c >= '0' && c <= '9');
    }
    std::cout << "  Custom salt (digits only): " << customSalt << std::endl;

    // Test with alphanumeric charset
    std::string alphanumericSalt = SaltGenerator::generateCustom(16,
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
    assert(alphanumericSalt.length() == 16);
    std::cout << "  Alphanumeric salt: " << alphanumericSalt << std::endl;

    // Test timestamped salt
    std::string timestampedSalt1 = SaltGenerator::generateTimestamped(32);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::string timestampedSalt2 = SaltGenerator::generateTimestamped(32);
    assert(timestampedSalt1 != timestampedSalt2); // Should be different due to timestamp
    assert(timestampedSalt1.length() == 32);
    std::cout << "  Timestamped salt: " << timestampedSalt1 << std::endl;

    // Test uniqueness - generate multiple salts and ensure they're different
    std::cout << "  Testing uniqueness..." << std::endl;
    std::set<std::string> uniqueSalts;
    for (int i = 0; i < 100; ++i) {
        uniqueSalts.insert(SaltGenerator::generateHex(16));
    }
    assert(uniqueSalts.size() == 100); // All should be unique

    // Test different lengths
    for (size_t len : {8, 16, 24, 32, 64}) {
        std::string salt = SaltGenerator::generateHex(len);
        assert(salt.length() == len * 2);
        assert(SaltGenerator::isValidHex(salt));
    }

    // Test validation functions
    assert(SaltGenerator::isValidHex("0123456789abcdef"));
    assert(SaltGenerator::isValidHex("ABCDEF0123456789"));
    assert(!SaltGenerator::isValidHex("xyz123"));
    assert(!SaltGenerator::isValidHex(""));

    assert(SaltGenerator::isValidBase64("SGVsbG8gV29ybGQ="));
    assert(SaltGenerator::isValidBase64("YWJjZGVmZ2hpams="));
    assert(!SaltGenerator::isValidBase64("Hello@World"));
    assert(!SaltGenerator::isValidBase64(""));

    // Test edge cases
    std::string emptySalt = SaltGenerator::generateHex(0);
    assert(emptySalt.empty());

    std::string emptyCustom = SaltGenerator::generateCustom(10, "");
    assert(emptyCustom.empty());

    std::string zeroLengthCustom = SaltGenerator::generateCustom(0, "abc");
    assert(zeroLengthCustom.empty());

    // Test randomness quality (basic statistical test)
    std::cout << "  Testing randomness distribution..." << std::endl;
    std::map<char, int> hexCharCounts;
    for (int i = 0; i < 1000; ++i) {
        std::string salt = SaltGenerator::generateHex(1);
        for (char c : salt) {
            hexCharCounts[c]++;
        }
    }

    // Each hex char (0-9, a-f) should appear roughly equally
    // With 2000 chars total (1000 salts * 2 chars), each should appear ~125 times
    for (const auto& [ch, count] : hexCharCounts) {
        assert(count > 50);  // Not too few
        assert(count < 200); // Not too many
    }

    // Performance test
    auto startPerf = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 1000; ++i) {
        SaltGenerator::generateHex(32);
    }
    auto endPerf = std::chrono::high_resolution_clock::now();
    auto perfTime = std::chrono::duration_cast<std::chrono::microseconds>(endPerf - startPerf).count();
    std::cout << "  Performance (1000 salts): " << perfTime << "μs ("
              << (perfTime / 1000.0) << "μs per salt)" << std::endl;

    // Test secure vs non-secure (both should work)
    std::string normalSalt = SaltGenerator::generateHex(16);
    std::string secureSalt = SaltGenerator::generateSecureHex(16);
    assert(normalSalt.length() == 32);
    assert(secureSalt.length() == 32);
    assert(normalSalt != secureSalt); // Should be different

    // Test bcrypt salt format
    for (int i = 0; i < 10; ++i) {
        std::string bcrypt = SaltGenerator::generateBcryptSalt();
        assert(bcrypt.length() == 22);
        // Verify it only contains bcrypt base64 chars
        for (char c : bcrypt) {
            bool valid = (c >= '0' && c <= '9') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') ||
                        c == '.' || c == '/';
            assert(valid);
        }
    }

    std::cout << "Salt Generator tests passed!" << std::endl;
}

// ==================== LoadBalancer Tests ====================

int main() {
    std::cout << "\n=== algorithm_test ===" << std::endl;
    try {
        testBase64();
        testMD5();
        testMurmurHash3();
        testSaltGenerator();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
