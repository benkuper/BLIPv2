#include "bench_runtime.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if BLIP_BENCH_WAMR
#include "wasm_export.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#elif BLIP_BENCH_WASM3
#include "wasm3.h"
#include "m3_env.h"
#endif

namespace bench {
namespace {
constexpr const char* names[] = {"noop", "integer", "float", "pixels", "host_calls", "trap", "invalid", "divide", "recursion", "grow", "spin"};
std::uint32_t calls{};
// Same native pixel scratch buffer in every variant; keep stores observable.
volatile std::uint32_t native_pixels[256]{};
int index_of(const char* name) {
    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (std::strcmp(name, names[i]) == 0) return static_cast<int>(i);
    return -1;
}
void copy_error(char* target, std::size_t capacity, const char* text) {
    std::snprintf(target, capacity, "%s", text ? text : "unknown error");
}
#if BLIP_BENCH_WAMR
void* pool{};
bool initialized{};
wasm_module_t module{};
wasm_module_inst_t instance{};
wasm_exec_env_t environment{};
wasm_function_inst_t functions[11]{};
std::uint32_t probe(wasm_exec_env_t, std::uint32_t value) { ++calls; return value + 1; }
#elif BLIP_BENCH_WASM3
IM3Environment environment{};
IM3Runtime runtime{};
IM3Function functions[11]{};
m3ApiRawFunction(probe) {
    m3ApiReturnType(std::uint32_t)
    m3ApiGetArg(std::uint32_t, value)
    (void)runtime; (void)_mem;
    ++calls;
    m3ApiReturn(value + 1);
}
#else
__attribute__((noinline)) std::uint32_t probe(std::uint32_t value) {
    ++calls;
    return value + 1;
}
#endif
}

std::uint32_t integer_work(std::uint32_t count) {
    std::uint32_t value = 42;
    while (count--) value = value * 1664525U + 1013904223U;
    return value;
}
std::uint32_t pixel_work(std::uint32_t count) {
    std::uint32_t sum{};
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto rgb = ((i & 255) << 16) | (((i * 3) & 255) << 8) | ((i * 7) & 255);
        if (i < 256) native_pixels[i] = rgb;
        sum += rgb;
    }
    return sum;
}
bool load(std::uint8_t* bytes, std::size_t length, char* error, std::size_t capacity) {
    calls = 0;
#if BLIP_BENCH_WAMR
    pool = std::malloc(pool_bytes);
    if (!pool) { copy_error(error, capacity, "pool allocation failed"); return false; }
    RuntimeInitArgs args{};
    args.mem_alloc_type = Alloc_With_Pool;
    args.mem_alloc_option.pool.heap_buf = pool;
    args.mem_alloc_option.pool.heap_size = pool_bytes;
    static NativeSymbol symbols[] = {{"probe", reinterpret_cast<void*>(probe), "(i)i", nullptr}};
    args.native_module_name = "blip";
    args.native_symbols = symbols;
    args.n_native_symbols = 1;
    initialized = wasm_runtime_full_init(&args);
    if (!initialized) { copy_error(error, capacity, "runtime initialization failed"); close(); return false; }
    module = wasm_runtime_load(bytes, static_cast<std::uint32_t>(length), error, capacity);
    if (module) instance = wasm_runtime_instantiate(module, wasm_stack_bytes, 0, error, capacity);
    if (instance) environment = wasm_runtime_create_exec_env(instance, wasm_stack_bytes);
    if (!environment) { if (instance) copy_error(error, capacity, "execution environment failed"); close(); return false; }
    // The bundled port scans the entire native stack on every call. The task
    // owns a fixed stack: obtain its base once without a watermark scan and
    // preserve 2 KiB for trap cleanup below the interpreter's boundary.
    TaskStatus_t task{};
    vTaskGetInfo(xTaskGetCurrentTaskHandle(), &task, pdFALSE, eInvalid);
    wasm_runtime_set_native_stack_boundary(environment, reinterpret_cast<std::uint8_t*>(task.pxStackBase) + 2048);
    for (std::size_t i = 0; i < 11; ++i) {
        functions[i] = wasm_runtime_lookup_function(instance, names[i]);
        if (!functions[i]) { copy_error(error, capacity, "missing export"); close(); return false; }
    }
#elif BLIP_BENCH_WASM3
    environment = m3_NewEnvironment();
    if (environment) runtime = m3_NewRuntime(environment, wasm_stack_bytes, nullptr);
    if (!runtime) { copy_error(error, capacity, "runtime initialization failed"); close(); return false; }
    runtime->memoryLimit = 65536;
    IM3Module module{};
    M3Result status = m3_ParseModule(environment, &module, bytes, static_cast<std::uint32_t>(length));
    if (!status) {
        status = m3_LoadModule(runtime, module);
        if (status) m3_FreeModule(module);
    }
    if (!status) status = m3_LinkRawFunction(module, "blip", "probe", "i(i)", probe);
    for (std::size_t i = 0; !status && i < 11; ++i) status = m3_FindFunction(&functions[i], runtime, names[i]);
    if (status) { copy_error(error, capacity, status); close(); return false; }
#else
    (void)bytes; (void)length; (void)error; (void)capacity;
#endif
    return true;
}
bool call(const char* name, std::uint32_t argument, std::uint32_t& result, char* error, std::size_t capacity, int fuel) {
    const int i = index_of(name);
    if (i < 0) { copy_error(error, capacity, "missing export"); return false; }
#if BLIP_BENCH_WAMR
    wasm_runtime_clear_exception(instance);
    wasm_runtime_set_instruction_count_limit(environment, fuel);
    std::uint32_t argv[] = {argument};
    if (!wasm_runtime_call_wasm(environment, functions[i], 1, argv)) {
        copy_error(error, capacity, wasm_runtime_get_exception(instance)); return false;
    }
    result = argv[0];
#elif BLIP_BENCH_WASM3
    (void)fuel;
    M3Result status = m3_CallV(functions[i], argument);
    if (!status) {
        if (i == 2) {
            float value{};
            status = m3_GetResultsV(functions[i], &value);
            std::memcpy(&result, &value, sizeof(value));
        } else status = m3_GetResultsV(functions[i], &result);
    }
    if (status) { copy_error(error, capacity, status); return false; }
#else
    (void)fuel;
    if (i == 0) result = argument;
    else if (i == 1) result = integer_work(argument);
    else if (i == 2) {
        float value{};
        for (std::uint32_t n = 0; n < argument; ++n) value += static_cast<float>(n & 255) * 0.125F;
        std::memcpy(&result, &value, sizeof(value));
    } else if (i == 3) result = pixel_work(argument);
    else if (i == 4) {
        result = 0;
        while (argument) result += probe(argument--);
    }
    else if (i == 9) result = 0xffffffffU;
    else { copy_error(error, capacity, "native baseline has no WASM faults"); return false; }
#endif
    return true;
}
void close() {
#if BLIP_BENCH_WAMR
    if (environment) wasm_runtime_destroy_exec_env(environment);
    if (instance) wasm_runtime_deinstantiate(instance);
    if (module) wasm_runtime_unload(module);
    if (initialized) wasm_runtime_destroy();
    std::free(pool);
    environment = nullptr; instance = nullptr; module = nullptr; pool = nullptr; initialized = false;
#elif BLIP_BENCH_WASM3
    if (runtime) m3_FreeRuntime(runtime);
    if (environment) m3_FreeEnvironment(environment);
    runtime = nullptr; environment = nullptr;
#endif
}
std::uint32_t reserved_bytes() {
#if BLIP_BENCH_WAMR
    return pool_bytes;
#else
    return 0;
#endif
}
std::uint32_t used_bytes() {
#if BLIP_BENCH_WAMR
    mem_alloc_info_t info{};
    if (wasm_runtime_get_mem_alloc_info(&info)) return info.total_size - info.total_free_size;
#endif
    return 0;
}
std::uint32_t peak_bytes() {
#if BLIP_BENCH_WAMR
    mem_alloc_info_t info{};
    if (wasm_runtime_get_mem_alloc_info(&info)) return info.highmark_size;
#endif
    return 0;
}
std::uint32_t probe_calls() { return calls; }
bool initial_memory_is_zero() {
#if BLIP_BENCH_WAMR
    if (!wasm_runtime_validate_app_addr(instance, 0, 65536)) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(wasm_runtime_addr_app_to_native(instance, 0));
#elif BLIP_BENCH_WASM3
    std::uint32_t length{};
    const auto* bytes = m3_GetMemory(runtime, &length, 0);
    if (length != 65536) return false;
#else
    return true;
#endif
#if BLIP_BENCH_WAMR || BLIP_BENCH_WASM3
    if (!bytes) return false;
    for (std::size_t i = 0; i < 65536; ++i) if (bytes[i] != 0) return false;
    return true;
#endif
}
}
