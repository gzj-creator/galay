module;

#include "module_prelude.hpp"

export module galay.kernel;

export {
#include "../common/defn.hpp"
#include "../common/error.h"
#include "../common/file_descriptor.h"
#include "../common/host.hpp"
#include "../common/handle_option.h"
#include "../../galay-utils/cache/bytes.hpp"
#include "../../galay-utils/cache/byte_queue_view.hpp"
#include "../common/buffer.h"
#include "../common/sleep.hpp"
#include "../common/logger.h"

namespace galay::kernel {
using ::galay::utils::ByteQueueView;
}

#include "../core/task.h"
#include "../core/scheduler.hpp"
#include "../core/io_scheduler.hpp"
#include "../core/compute_scheduler.h"
#include "../core/runtime.h"
#include "../core/timer_scheduler.h"

#include "../concurrency/mpsc_channel.h"
#include "../concurrency/unsafe_channel.h"
#include "../concurrency/async_mutex.h"
#include "../concurrency/async_waiter.h"

#include "../async/tcp_socket.h"
#include "../async/udp_socket.h"
#include "../async/file_watcher.h"

#if defined(USE_KQUEUE) || defined(USE_IOURING)
#include "../async/async_file.h"
#endif

#ifdef USE_EPOLL
#include "../async/aio_file.h"
#endif
}
