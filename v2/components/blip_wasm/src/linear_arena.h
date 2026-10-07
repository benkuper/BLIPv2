#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Exclusive runtime owner/worker only; no engine handles escape. */
void blip_wasm_linear_arena(void *memory, size_t bytes);
size_t blip_wasm_linear_used(void);
size_t blip_wasm_linear_peak(void);
#ifdef __cplusplus
}
#endif
