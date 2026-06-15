module;

#include "galay-kernel/module/module_prelude.hpp"

export module galay.kernel;

export {
#include "galay-kernel/common/defn.hpp"
#include "galay-kernel/common/error.h"
#include "galay-kernel/common/host.hpp"
#include "galay-kernel/common/handle_option.h"
#include <galay-utils/cache/bytes.hpp>
#include <galay-utils/cache/byte_queue_view.hpp>
#include "galay-kernel/common/buffer.h"
#include "galay-kernel/common/sleep.hpp"
#include "galay-kernel/common/logger.h"

namespace galay::kernel {
using ::galay::utils::ByteQueueView;
}

#include "galay-kernel/core/task.h"
#include "galay-kernel/core/scheduler.hpp"
#include "galay-kernel/core/io_scheduler.hpp"
#include "galay-kernel/core/compute_scheduler.h"
#include "galay-kernel/core/runtime.h"
#include "galay-kernel/core/timer_scheduler.h"

#include "galay-kernel/concurrency/mpsc_channel.h"
#include "galay-kernel/concurrency/unsafe_channel.h"
#include "galay-kernel/concurrency/async_mutex.h"
#include "galay-kernel/concurrency/async_waiter.h"

#include "galay-kernel/async/tcp_socket.h"
#include "galay-kernel/async/udp_socket.h"
#include "galay-kernel/async/file_watcher.h"

#if defined(USE_KQUEUE) || defined(USE_IOURING)
#include "galay-kernel/async/async_file.h"
#endif

#ifdef USE_EPOLL
#include "galay-kernel/async/aio_file.h"
#endif
}
