module;

#include "kernel/module/module_prelude.hpp"

export module galay.kernel;

export {
#include "kernel/common/defn.hpp"
#include "kernel/common/error.h"
#include "kernel/common/host.hpp"
#include "kernel/common/handle_option.h"
#include <utils/cache/bytes.hpp>
#include <utils/cache/byte_queue_view.hpp>
#include "kernel/common/buffer.h"
#include "kernel/common/sleep.hpp"
#include "kernel/common/logger.h"

namespace galay::kernel {
using ::galay::utils::ByteQueueView;
}

#include "kernel/kernel/task.h"
#include "kernel/kernel/scheduler.hpp"
#include "kernel/kernel/io_scheduler.hpp"
#include "kernel/kernel/compute_scheduler.h"
#include "kernel/kernel/runtime.h"
#include "kernel/kernel/timer_scheduler.h"

#include "kernel/concurrency/mpsc_channel.h"
#include "kernel/concurrency/unsafe_channel.h"
#include "kernel/concurrency/async_mutex.h"
#include "kernel/concurrency/async_waiter.h"

#include "kernel/async/tcp_socket.h"
#include "kernel/async/udp_socket.h"
#include "kernel/async/file_watcher.h"

#if defined(USE_KQUEUE) || defined(USE_IOURING)
#include "kernel/async/async_file.h"
#endif

#ifdef USE_EPOLL
#include "kernel/async/aio_file.h"
#endif
}
