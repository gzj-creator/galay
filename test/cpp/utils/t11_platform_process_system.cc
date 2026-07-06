#include "test_common.hpp"

void test_system() {
    std::cout << "=== Testing System ===" << std::endl;

    // File operations
    static std::atomic_uint64_t testPathCounter{0};
    const std::string testSuffix = std::to_string(Time::currentTimeNs()) + "_" +
                                   std::to_string(testPathCounter.fetch_add(1));
    std::string testFile = "/tmp/galay_test_file_" + testSuffix + ".txt";
    assert(System::writeFile(testFile, "Hello, World!"));
    assert(System::fileExists(testFile));

    auto content = System::readFile(testFile);
    assert(content.has_value());
    assert(*content == "Hello, World!");

    assert(System::fileSize(testFile) == 13);
    assert(System::remove(testFile));
    assert(!System::fileExists(testFile));

    // Directory
    std::string testDir = "/tmp/galay_test_dir_" + testSuffix;
    assert(System::createDirectory(testDir));
    assert(System::isDirectory(testDir));
    assert(System::remove(testDir));

    // Environment
    System::setEnv("GALAY_TEST_VAR", "test_value");
    assert(System::getEnv("GALAY_TEST_VAR") == "test_value");
    System::unsetEnv("GALAY_TEST_VAR");
    assert(System::getEnv("GALAY_TEST_VAR", "default") == "default");

    // System info
    assert(System::cpuCount() > 0);
    assert(!System::hostname().empty());
    assert(!System::currentDir().empty());

    std::cout << "  CPU count: " << System::cpuCount() << std::endl;
    std::cout << "  Hostname: " << System::hostname() << std::endl;

    // Edge cases for file operations
    // Reading non-existent file
    auto nonExistent = System::readFile("/tmp/non_existent_file.txt");
    assert(!nonExistent.has_value());

    // Writing empty content
    assert(System::writeFile("/tmp/empty_file.txt", ""));
    assert(System::fileExists("/tmp/empty_file.txt"));
    assert(System::fileSize("/tmp/empty_file.txt") == 0);
    System::remove("/tmp/empty_file.txt");

    // Environment variables with empty values
    System::setEnv("GALAY_EMPTY_VAR", "");
    assert(System::getEnv("GALAY_EMPTY_VAR") == "");
    System::unsetEnv("GALAY_EMPTY_VAR");

    std::cout << "System tests passed!" << std::endl;
}

// ==================== Time Utility Tests ====================

void test_back_trace() {
    std::cout << "=== Testing BackTrace ===" << std::endl;

    auto frames = BackTrace::getStackTrace(10, 0);
    assert(!frames.empty());

    std::string traceStr = BackTrace::getStackTraceString(5, 0);
    assert(!traceStr.empty());

    std::cout << "  Got " << frames.size() << " stack frames" << std::endl;
    std::cout << "BackTrace tests passed!" << std::endl;
}

// ==================== SignalHandler Tests ====================

void test_signal_handler() {
    std::cout << "=== Testing SignalHandler ===" << std::endl;

    auto& handler = SignalHandler::instance();

    bool signalReceived = false;
    handler.setHandler(SIGUSR1, [&signalReceived](int) {
        signalReceived = true;
    });

    assert(handler.hasHandler(SIGUSR1));

    // Send signal to self
    raise(SIGUSR1);
    assert(signalReceived);

    handler.removeHandler(SIGUSR1);
    assert(!handler.hasHandler(SIGUSR1));

    std::cout << "SignalHandler tests passed!" << std::endl;
}

// ==================== Pool Tests ====================

void test_process() {
    std::cout << "=== Testing Process ===" << std::endl;

    // Current process info
    ProcessId pid = Process::currentId();
    assert(pid > 0);

    ProcessId ppid = Process::parentId();
    assert(ppid > 0);

    std::cout << "  Current PID: " << pid << std::endl;
    std::cout << "  Parent PID: " << ppid << std::endl;

    // Execute command
    auto [status, output] = Process::executeWithOutput("echo hello");
    assert(status.success());
    assert(output.find("hello") != std::string::npos);

    // Check if process is running
    assert(Process::isRunning(pid));

    auto priority = Process::priority(pid);
    assert(priority.has_value());

    auto samePriority = Process::setPriority(pid, *priority);
    assert(samePriority.has_value());

    auto updatedPriority = Process::priority(pid);
    assert(updatedPriority.has_value());
    assert(*updatedPriority == *priority);

    auto invalidPriority = Process::setPriority(pid, 20);
    assert(!invalidPriority.has_value());
    assert(invalidPriority.error() == ProcessPriorityError::InvalidPriority);

    std::cout << "Process tests passed!" << std::endl;
}

// ==================== TypeName Tests ====================

int main() {
    std::cout << "\n=== platform_test ===" << std::endl;
    try {
        test_system();
        test_back_trace();
        test_signal_handler();
        test_process();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
