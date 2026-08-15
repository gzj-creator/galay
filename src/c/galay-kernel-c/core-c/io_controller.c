#include "io_controller.h"

#include <string.h>

static C_IOResult make_result(C_IOResultCode code, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, 0, NULL};
}

C_IOResult galay_c_io_controller_init(galay_c_io_controller_t* controller,
                                      int fd,
                                      void* user_data)
{
    if (controller == NULL || fd < 0) {
        return make_result(C_IOResultInvalid, 0);
    }

    memset(controller, 0, sizeof(*controller));
    controller->fd = fd;
    controller->user_data = user_data;
    atomic_init(&controller->read_slot, NULL);
    atomic_init(&controller->write_slot, NULL);
    atomic_init(&controller->registered_events, GALAY_C_EVENT_NONE);
    atomic_init(&controller->owner_scheduler, NULL);

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_io_controller_cleanup(galay_c_io_controller_t* controller)
{
    if (controller == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }

    // 检查是否还有挂起的协程
    if (atomic_load_explicit(&controller->read_slot, memory_order_acquire) != NULL ||
        atomic_load_explicit(&controller->write_slot, memory_order_acquire) != NULL) {
        return make_result(C_IOResultInvalid, 0);
    }

    // 清理状态
    atomic_store_explicit(&controller->registered_events, GALAY_C_EVENT_NONE, memory_order_release);
    atomic_store_explicit(&controller->owner_scheduler, NULL, memory_order_release);
    controller->fd = -1;
    controller->user_data = NULL;

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_io_controller_register_read(galay_c_io_controller_t* controller,
                                                C_CoroTaskInternal* coro,
                                                galay_c_io_scheduler_t* scheduler)
{
    if (controller == NULL || coro == NULL || scheduler == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }

    // 验证 scheduler 所有权
    galay_c_io_scheduler_t* expected_sched = NULL;
    if (!atomic_compare_exchange_strong_explicit(&controller->owner_scheduler,
                                                   &expected_sched,
                                                   scheduler,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire)) {
        // 已有 owner，检查是否匹配
        if (expected_sched != scheduler) {
            return make_result(C_IOResultInvalid, 0);
        }
    }

    // 原子注册到读槽位
    C_CoroTaskInternal* expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(&controller->read_slot,
                                                   &expected,
                                                   coro,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire)) {
        return make_result(C_IOResultInvalid, 0);
    }

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_io_controller_register_write(galay_c_io_controller_t* controller,
                                                 C_CoroTaskInternal* coro,
                                                 galay_c_io_scheduler_t* scheduler)
{
    if (controller == NULL || coro == NULL || scheduler == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }

    // 验证 scheduler 所有权
    galay_c_io_scheduler_t* expected_sched = NULL;
    if (!atomic_compare_exchange_strong_explicit(&controller->owner_scheduler,
                                                   &expected_sched,
                                                   scheduler,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire)) {
        if (expected_sched != scheduler) {
            return make_result(C_IOResultInvalid, 0);
        }
    }

    // 原子注册到写槽位
    C_CoroTaskInternal* expected = NULL;
    if (!atomic_compare_exchange_strong_explicit(&controller->write_slot,
                                                   &expected,
                                                   coro,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire)) {
        return make_result(C_IOResultInvalid, 0);
    }

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_io_controller_clear_read(galay_c_io_controller_t* controller,
                                             C_CoroTaskInternal* expected_coro)
{
    if (controller == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }

    if (expected_coro == NULL) {
        // 无条件清除
        C_CoroTaskInternal* prev = atomic_exchange_explicit(&controller->read_slot,
                                                             NULL,
                                                             memory_order_acq_rel);
        return prev != NULL ? make_result(C_IOResultOk, 0) : make_result(C_IOResultInvalid, 0);
    }

    // 条件清除：只有匹配时才清除
    if (!atomic_compare_exchange_strong_explicit(&controller->read_slot,
                                                   &expected_coro,
                                                   NULL,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire)) {
        return make_result(C_IOResultInvalid, 0);
    }

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_io_controller_clear_write(galay_c_io_controller_t* controller,
                                              C_CoroTaskInternal* expected_coro)
{
    if (controller == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }

    if (expected_coro == NULL) {
        C_CoroTaskInternal* prev = atomic_exchange_explicit(&controller->write_slot,
                                                             NULL,
                                                             memory_order_acq_rel);
        return prev != NULL ? make_result(C_IOResultOk, 0) : make_result(C_IOResultInvalid, 0);
    }

    if (!atomic_compare_exchange_strong_explicit(&controller->write_slot,
                                                   &expected_coro,
                                                   NULL,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire)) {
        return make_result(C_IOResultInvalid, 0);
    }

    return make_result(C_IOResultOk, 0);
}

int galay_c_io_controller_has_read_coro(const galay_c_io_controller_t* controller,
                                        const C_CoroTaskInternal* coro)
{
    if (controller == NULL || coro == NULL) {
        return 0;
    }

    const C_CoroTaskInternal* current = atomic_load_explicit(&controller->read_slot,
                                                              memory_order_acquire);
    return current == coro;
}

int galay_c_io_controller_has_write_coro(const galay_c_io_controller_t* controller,
                                         const C_CoroTaskInternal* coro)
{
    if (controller == NULL || coro == NULL) {
        return 0;
    }

    const C_CoroTaskInternal* current = atomic_load_explicit(&controller->write_slot,
                                                              memory_order_acquire);
    return current == coro;
}
