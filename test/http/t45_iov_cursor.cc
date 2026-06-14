#include "http/common/iovec_utils.h"
#include <array>
#include <iostream>
#include <span>

int main() {
    std::array<char, 4> first{'a', 'b', 'c', 'd'};
    std::array<char, 3> second{'e', 'f', 'g'};

    galay::kernel::IoVecWriteState state;
    state.reserve(3);
    state.append({
        .iov_base = first.data(),
        .iov_len = first.size()
    });
    state.append({
        .iov_base = nullptr,
        .iov_len = 0
    });
    state.append({
        .iov_base = second.data(),
        .iov_len = second.size()
    });

    if (state.count() != 2) {
        std::cerr << "[T45] expected normalized count=2, got " << state.count() << "\n";
        return 1;
    }

    if (state.data() == nullptr || state.data()[0].iov_base != first.data() || state.data()[0].iov_len != first.size()) {
        std::cerr << "[T45] unexpected first window after normalize\n";
        return 1;
    }

    const size_t advanced = state.advance(5);
    if (advanced != 5) {
        std::cerr << "[T45] expected advance=5, got " << advanced << "\n";
        return 1;
    }

    if (state.count() != 1) {
        std::cerr << "[T45] expected remaining count=1, got " << state.count() << "\n";
        return 1;
    }

    if (state.data()[0].iov_base != (second.data() + 1) || state.data()[0].iov_len != 2) {
        std::cerr << "[T45] unexpected current window after advance\n";
        return 1;
    }

    std::vector<struct iovec> window;
    state.exportWindow(window);
    if (window.size() != 1 || window[0].iov_base != (second.data() + 1) || window[0].iov_len != 2) {
        std::cerr << "[T45] unexpected exported window after advance\n";
        return 1;
    }

    std::cout << "T45-IoVecWriteState PASS\n";
    return 0;
}
