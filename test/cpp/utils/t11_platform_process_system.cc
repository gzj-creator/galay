#include "test_common.hpp"
#include <limits>

void test_system() {
    std::cout << "=== Testing System ===" << std::endl;

    // File operations
    static std::atomic_uint64_t testPathCounter{0};
    const std::string testSuffix = std::to_string(Time::current_time_ns()) + "_" +
                                   std::to_string(testPathCounter.fetch_add(1));
    std::string testFile = "/tmp/galay_test_file_" + testSuffix + ".txt";
    assert(System::write_file(testFile, "Hello, World!"));
    assert(System::file_exists(testFile));

    auto content = System::read_file(testFile);
    assert(content.has_value());
    assert(*content == "Hello, World!");

    assert(System::file_size(testFile) == 13);
    assert(System::remove(testFile));
    assert(!System::file_exists(testFile));

    // Directory
    std::string testDir = "/tmp/galay_test_dir_" + testSuffix;
    assert(System::create_directory(testDir));
    assert(System::is_directory(testDir));
    assert(System::remove(testDir));

    // System info
    if (CPU::count() != std::thread::hardware_concurrency()) {
        std::cerr << "CPU::count must preserve the hardware concurrency hint\n";
        std::exit(1);
    }
    assert(!System::hostname().empty());
    assert(!System::current_dir().empty());

    std::cout << "  CPU count: " << CPU::count() << std::endl;
    std::cout << "  Hostname: " << System::hostname() << std::endl;

    // Edge cases for file operations
    // Reading non-existent file
    auto nonExistent = System::read_file("/tmp/non_existent_file.txt");
    assert(!nonExistent.has_value());

    // Writing empty content
    assert(System::write_file("/tmp/empty_file.txt", ""));
    assert(System::file_exists("/tmp/empty_file.txt"));
    assert(System::file_size("/tmp/empty_file.txt") == 0);
    System::remove("/tmp/empty_file.txt");

    std::cout << "System tests passed!" << std::endl;
}

// ==================== Time Utility Tests ====================

void test_back_trace() {
    std::cout << "=== Testing BackTrace ===" << std::endl;

    auto frames = BackTrace::get_stack_trace(10, 0);
    assert(!frames.empty());

    std::string traceStr = BackTrace::get_stack_trace_string(5, 0);
    assert(!traceStr.empty());

    std::cout << "  Got " << frames.size() << " stack frames" << std::endl;
    std::cout << "BackTrace tests passed!" << std::endl;
}

// ==================== SignalHandler Tests ====================

void test_signal_handler() {
    std::cout << "=== Testing SignalHandler ===" << std::endl;

    auto& handler = SignalHandler::instance();

    bool signalReceived = false;
    handler.set_handler(SIGUSR1, [&signalReceived](int) {
        signalReceived = true;
    });

    assert(handler.has_handler(SIGUSR1));

    // Send signal to self
    raise(SIGUSR1);
    assert(signalReceived);

    handler.remove_handler(SIGUSR1);
    assert(!handler.has_handler(SIGUSR1));

    std::cout << "SignalHandler tests passed!" << std::endl;
}

// ==================== Pool Tests ====================

void test_process_priority_errors() {
    using galay::utils::ProcessPriorityError;

    const ProcessPriorityError errors[] = {
        ProcessPriorityError::InvalidPriority,
        ProcessPriorityError::PermissionDenied,
        ProcessPriorityError::NotFound,
        ProcessPriorityError::SystemError,
    };

    for (ProcessPriorityError error : errors) {
        assert(process_priority_error_string(error)[0] != '\0');
    }
}

void test_process_affinity() {
    ProcessId pid = Process::current_id();
    auto original = Process::cpu_affinity(pid);

#if !defined(_WIN32) && !defined(__linux__)
    if (original || original.error() != ProcessAffinityError::Unsupported) {
        std::exit(1);
    }
#else
    if (!original || original->empty()) {
        std::exit(1);
    }

#if defined(__linux__)
    const auto current = CPU::cpu_affinity();
    if (!current || *current != *original) {
        std::cerr << "Process must enumerate the entire actual Linux mask\n";
        std::exit(1);
    }
#endif

    auto sameAffinity = Process::set_cpu_affinity(pid, *original);
    auto after = Process::cpu_affinity(pid);
    if (!sameAffinity || !after || *after != *original) {
        std::exit(1);
    }
#endif

    const std::array<unsigned int, 0> empty{};
    auto emptyResult = Process::set_cpu_affinity(pid, empty);
    if (emptyResult || emptyResult.error() != ProcessAffinityError::EmptyCpuSet) {
        std::exit(1);
    }

    const std::array<unsigned int, 1> invalid{std::numeric_limits<unsigned>::max()};
    auto invalidResult = Process::set_cpu_affinity(pid, invalid);
#if defined(_WIN32) || defined(__linux__)
    if (invalidResult || invalidResult.error() != ProcessAffinityError::InvalidCpu) {
        std::exit(1);
    }
#else
    if (invalidResult || invalidResult.error() != ProcessAffinityError::Unsupported) {
        std::exit(1);
    }
#endif

    const ProcessAffinityError errors[] = {
        ProcessAffinityError::EmptyCpuSet,
        ProcessAffinityError::InvalidCpu,
        ProcessAffinityError::PermissionDenied,
        ProcessAffinityError::NotFound,
        ProcessAffinityError::Unsupported,
        ProcessAffinityError::SystemError,
    };
    for (ProcessAffinityError error : errors) {
        assert(process_affinity_error_string(error)[0] != '\0');
    }
}

void test_process() {
    std::cout << "=== Testing Process ===" << std::endl;

    // Current process info
    ProcessId pid = Process::current_id();
    assert(pid > 0);

    ProcessId ppid = Process::parent_id();
    assert(ppid > 0);

    std::cout << "  Current PID: " << pid << std::endl;
    std::cout << "  Parent PID: " << ppid << std::endl;

    // Execute command
    auto [status, output] = Process::execute_with_output("echo hello");
    assert(status.success());
    assert(output.find("hello") != std::string::npos);

    // Check if process is running
    assert(Process::is_running(pid));

    auto priority = Process::priority(pid);
    assert(priority.has_value());

    auto samePriority = Process::set_priority(pid, *priority);
    assert(samePriority.has_value());

    auto updatedPriority = Process::priority(pid);
    assert(updatedPriority.has_value());
    assert(*updatedPriority == *priority);

    auto invalidPriority = Process::set_priority(pid, 20);
    assert(!invalidPriority.has_value());
    assert(invalidPriority.error() == ProcessPriorityError::InvalidPriority);

    test_process_priority_errors();
    test_process_affinity();

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
