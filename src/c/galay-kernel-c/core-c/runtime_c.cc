#include "runtime_c.h"

#include "../../../cpp/galay-kernel/core/runtime.h"

namespace
{

galay::kernel::Runtime* to_cpp_runtime(galay_kernel_runtime_t* c_runtime)
{
    return static_cast<galay::kernel::Runtime*>(c_runtime->runtime);
}

const galay::kernel::Runtime* to_cpp_runtime(const galay_kernel_runtime_t* c_runtime)
{
    return static_cast<const galay::kernel::Runtime*>(c_runtime->runtime);
}

galay::kernel::RuntimeConfig to_cpp_config(const C_RuntimeConfig& config)
{
    galay::kernel::RuntimeConfig cpp_config;
    cpp_config.io_scheduler_count = config.io_scheduler_count;
    cpp_config.compute_scheduler_count = config.compute_scheduler_count;
    return cpp_config;
}

} // namespace

extern "C" {

const char* galay_kernel_runtime_get_error(C_RuntimeResultCode code)
{
    switch (code) {
    case C_RuntimeSuccess:
        return "success";
    case C_RuntimeParameterInvalid:
        return "parameter invalid";
    case C_RuntimeMemoryAllocFailed:
        return "memory allocation failed";
    case C_RuntimeStartFailed:
        return "runtime start failed";
    }
    return "unknown runtime error";
}

C_RuntimeConfig galay_kernel_runtime_config_default(void)
{
    return C_RuntimeConfig{
        C_RUNTIME_SCHEDULER_COUNT_AUTO,
        C_RUNTIME_SCHEDULER_COUNT_AUTO
    };
}

C_RuntimeResultCode galay_kernel_runtime_create(
    const C_RuntimeConfig* config,
    galay_kernel_runtime_t* c_runtime)
{
    if (config == nullptr || c_runtime == nullptr) {
        return C_RuntimeParameterInvalid;
    }

    c_runtime->runtime = nullptr;
    auto* runtime = new (std::nothrow) galay::kernel::Runtime(to_cpp_config(*config));
    if (runtime == nullptr) {
        return C_RuntimeMemoryAllocFailed;
    }

    c_runtime->runtime = runtime;
    return C_RuntimeSuccess;
}

C_RuntimeResultCode galay_kernel_runtime_start(galay_kernel_runtime_t* c_runtime)
{
    if (c_runtime == nullptr || c_runtime->runtime == nullptr) {
        return C_RuntimeParameterInvalid;
    }

    auto started = to_cpp_runtime(c_runtime)->start();
    return started ? C_RuntimeSuccess : C_RuntimeStartFailed;
}

C_RuntimeResultCode galay_kernel_runtime_stop(galay_kernel_runtime_t* c_runtime)
{
    if (c_runtime == nullptr || c_runtime->runtime == nullptr) {
        return C_RuntimeParameterInvalid;
    }

    to_cpp_runtime(c_runtime)->stop();
    return C_RuntimeSuccess;
}

bool galay_kernel_runtime_is_running(const galay_kernel_runtime_t* c_runtime)
{
    if (c_runtime == nullptr || c_runtime->runtime == nullptr) {
        return false;
    }

    return to_cpp_runtime(c_runtime)->isRunning();
}

C_RuntimeResultCode galay_kernel_runtime_destroy(galay_kernel_runtime_t* c_runtime)
{
    if (c_runtime == nullptr) {
        return C_RuntimeParameterInvalid;
    }

    delete to_cpp_runtime(c_runtime);
    c_runtime->runtime = nullptr;
    return C_RuntimeSuccess;
}

} // extern "C"
