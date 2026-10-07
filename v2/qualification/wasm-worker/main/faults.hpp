#pragma once
#include <cstdint>
namespace qualification {
enum class Fault : std::uint8_t { none, engine_pool, module_buffer, linear_arena,
                                 supervisor_task, pthread_argument, pthread_record, worker_task };
void arm(Fault fault) noexcept;
unsigned hits() noexcept;
bool consumed() noexcept;
} // namespace qualification
