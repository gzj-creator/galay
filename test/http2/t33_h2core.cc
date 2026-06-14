/**
 * @file T33-H2ConnectionCoreLifecycle.cc
 * @brief HTTP/2 connection core lifecycle contract test
 */

#include "http2/kernel/h2_core.h"
#include <cassert>
#include <iostream>

using namespace galay::http2;

int main() {
    Http2ConnectionCore core;
    assert(core.state() == Http2ConnectionCore::State::Idle);

    core.markSettingsSent();
    assert(core.isSettingsAckPending());

    core.markSettingsAcked();
    assert(!core.isSettingsAckPending());

    core.requestStop();
    assert(core.stopRequested());

    std::cout << "T33-H2ConnectionCoreLifecycle PASS\n";
    return 0;
}
