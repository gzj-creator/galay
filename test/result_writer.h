#ifndef GALAY_TEST_RESULT_WRITER_H
#define GALAY_TEST_RESULT_WRITER_H

#include <string>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace galay::test {

/**
 * @brief 测试结果写入器
 *
 * 用于将测试结果写入文件，方便自动化检查
 */
class TestResultWriter {
public:
    TestResultWriter(const std::string& test_name)
        : m_test_name(test_name)
        , m_passed(0)
        , m_failed(0)
        , m_total(0)
    {
        m_start_time = std::chrono::steady_clock::now();
    }

    void addTest() {
        m_total++;
    }

    void addPassed() {
        m_passed++;
    }

    void addFailed() {
        m_failed++;
    }

    void writeResult(const std::string& output_dir = "test_results") {
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - m_start_time).count();

        // 创建输出目录
        std::string mkdir_cmd = "mkdir -p " + output_dir;
        if (system(mkdir_cmd.c_str()) != 0) {}

        // 写入结果文件
        std::string filename = output_dir + "/" + m_test_name + ".result";
        std::ofstream ofs(filename);

        if (!ofs.is_open()) {
            return;
        }

        // 写入结果
        ofs << "TEST_NAME=" << m_test_name << "\n";
        ofs << "TOTAL=" << m_total << "\n";
        ofs << "PASSED=" << m_passed << "\n";
        ofs << "FAILED=" << m_failed << "\n";
        ofs << "DURATION_MS=" << duration << "\n";

        if (m_failed == 0 && m_total > 0) {
            ofs << "STATUS=PASS\n";
        } else {
            ofs << "STATUS=FAIL\n";
        }

        ofs.close();
    }

    int getPassed() const { return m_passed; }
    int getFailed() const { return m_failed; }
    int getTotal() const { return m_total; }

private:
    std::string m_test_name;
    int m_passed;
    int m_failed;
    int m_total;
    std::chrono::steady_clock::time_point m_start_time;
};

} // namespace galay::test

#endif // GALAY_TEST_RESULT_WRITER_H
