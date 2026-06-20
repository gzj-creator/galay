#ifndef RESULT_WRITER_H
#define RESULT_WRITER_H

#include <fstream>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace test
{

class TestResultWriter {
public:
    explicit TestResultWriter(const std::string& filename)
        : m_file(filename, std::ios::out | std::ios::trunc) {
        if (m_file.is_open()) {
            m_file << "# Test Results\n";
            m_file << "# Generated at: " << currentTime() << "\n\n";
        }
    }

    ~TestResultWriter() {
        if (m_file.is_open()) {
            m_file.close();
        }
    }

    void writeTestCase(const std::string& name, bool passed, const std::string& message = "") {
        if (m_file.is_open()) {
            m_file << "[" << (passed ? "PASS" : "FAIL") << "] " << name;
            if (!message.empty()) {
                m_file << " - " << message;
            }
            m_file << "\n";
            m_file.flush();
        }

        if (passed) {
            ++m_passed;
        } else {
            ++m_failed;
        }
    }

    void writeSummary() {
        if (m_file.is_open()) {
            m_file << "\n# Summary\n";
            m_file << "Total: " << (m_passed + m_failed) << "\n";
            m_file << "Passed: " << m_passed << "\n";
            m_file << "Failed: " << m_failed << "\n";
            m_file.flush();
        }
    }

    int passed() const { return m_passed; }
    int failed() const { return m_failed; }

private:
    std::string currentTime() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }

    std::ofstream m_file;
    int m_passed = 0;
    int m_failed = 0;
};

} // namespace test

#endif // RESULT_WRITER_H
