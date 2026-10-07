#include "faults.hpp"
#include "blip/wasm/esp_wasm_component.hpp"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

namespace {
using qualification::Fault;
std::atomic<Fault> armed{Fault::none};
std::atomic<unsigned> hit_count{};
std::atomic<TaskHandle_t> creating_task{};
unsigned parent_callocs{};
bool consume(Fault fault) {
    if (!armed.compare_exchange_strong(fault, Fault::none)) return false;
    hit_count.fetch_add(1);
    return true;
}
bool named(const char* name, const char* expected) { return name && std::strcmp(name, expected) == 0; }
}
namespace qualification {
void arm(Fault fault) noexcept { hit_count.store(0); armed.store(fault); }
unsigned hits() noexcept { return hit_count.load(); }
bool consumed() noexcept { return armed.load() == Fault::none; }
}
extern "C" {
void* __real_heap_caps_aligned_alloc(std::size_t, std::size_t, std::uint32_t);
void* __wrap_heap_caps_aligned_alloc(std::size_t alignment, std::size_t size, std::uint32_t caps) {
    if (size == blip::wasm::EspWasmComponent::kEnginePoolBytes && consume(Fault::engine_pool)) return nullptr;
    return __real_heap_caps_aligned_alloc(alignment, size, caps);
}
void* __real_heap_caps_malloc(std::size_t, std::uint32_t);
void* __wrap_heap_caps_malloc(std::size_t size, std::uint32_t caps) {
    if (size == blip::wasm::EspWasmComponent::kModuleBytes && consume(Fault::module_buffer)) return nullptr;
    if (size == 65536 && (caps & MALLOC_CAP_IRAM_8BIT) && consume(Fault::linear_arena)) return nullptr;
    return __real_heap_caps_malloc(size, caps);
}
void* __real_calloc(std::size_t, std::size_t);
void* __wrap_calloc(std::size_t count, std::size_t size) {
    // S3 can initialize WAMR on another core while pthread_create returns.
    // Inject only into the caller's actual SDK pthread allocation sequence.
    if (creating_task.load() == xTaskGetCurrentTaskHandle()) {
        ++parent_callocs;
        if ((parent_callocs == 1 && consume(Fault::pthread_argument)) ||
            (parent_callocs == 2 && consume(Fault::pthread_record))) return nullptr;
    }
    return __real_calloc(count, size);
}
int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*entry)(void*), void* arg) {
    parent_callocs = 0;
    creating_task.store(xTaskGetCurrentTaskHandle());
    const int result = __real_pthread_create(thread, attr, entry, arg);
    creating_task.store(nullptr);
    return result;
}
BaseType_t __real_xTaskCreate(TaskFunction_t, const char*, configSTACK_DEPTH_TYPE, void*, UBaseType_t, TaskHandle_t*);
BaseType_t __wrap_xTaskCreate(TaskFunction_t entry, const char* name, configSTACK_DEPTH_TYPE stack,
                             void* arg, UBaseType_t priority, TaskHandle_t* handle) {
    if (named(name, "blip_wasm_guard") && consume(Fault::supervisor_task)) return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    return __real_xTaskCreate(entry, name, stack, arg, priority, handle);
}
BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t, const char*, configSTACK_DEPTH_TYPE, void*, UBaseType_t, TaskHandle_t*, BaseType_t);
BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t entry, const char* name, configSTACK_DEPTH_TYPE stack,
                                         void* arg, UBaseType_t priority, TaskHandle_t* handle, BaseType_t core) {
    if (named(name, "blip_wasm_guard") && consume(Fault::supervisor_task)) return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    if (named(name, "blip_wasm") && consume(Fault::worker_task)) return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    return __real_xTaskCreatePinnedToCore(entry, name, stack, arg, priority, handle, core);
}
}
