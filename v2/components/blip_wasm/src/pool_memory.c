/* Linear mappings use the declared engine pool or an exclusive borrowed arena.
 * The runtime owner configures this state before engine initialization and
 * clears it only after shutdown; mapping operations are worker-confined. */
#include "platform_api_vmcore.h"
#include "wasm_export.h"
#include "linear_arena.h"

static void *linear_memory;
static size_t linear_capacity, linear_used, linear_peak;
static bool linear_mapped;

void blip_wasm_linear_arena(void *memory, size_t bytes)
{
    linear_memory = memory;
    linear_capacity = bytes;
    linear_used = linear_peak = 0;
    linear_mapped = false;
}
size_t blip_wasm_linear_used(void) { return linear_used; }
size_t blip_wasm_linear_peak(void) { return linear_peak; }

void *os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
    (void)flags;
    (void)file;
    if (hint || (prot & MMAP_PROT_EXEC) || size > 65536) return NULL;
    void *memory;
    if (linear_memory) {
        if (linear_mapped || size > linear_capacity) return NULL;
        linear_mapped = true;
        linear_used = size;
        if (size > linear_peak) linear_peak = size;
        memory = linear_memory;
    } else memory = wasm_runtime_malloc((unsigned int)size);
    if (memory) memset(memory, 0, size);
    return memory;
}
void *os_mremap(void *old_addr, size_t old_size, size_t new_size)
{
    if (new_size > 65536) return NULL;
    if (linear_memory) {
        if (!linear_mapped || old_addr != linear_memory || old_size != linear_used || new_size > linear_capacity) return NULL;
        if (new_size > old_size) memset((uint8_t *)old_addr + old_size, 0, new_size - old_size);
        linear_used = new_size;
        if (new_size > linear_peak) linear_peak = new_size;
        return old_addr;
    }
    void *memory = wasm_runtime_realloc(old_addr, (unsigned int)new_size);
    if (memory && new_size > old_size) memset((uint8_t *)memory + old_size, 0, new_size - old_size);
    return memory;
}
void os_munmap(void *addr, size_t size)
{
    (void)size;
    if (linear_memory && addr == linear_memory) {
        linear_mapped = false;
        linear_used = 0;
    } else wasm_runtime_free(addr);
}
int os_mprotect(void *addr, size_t size, int prot)
{
    (void)addr;
    (void)size;
    return (prot & MMAP_PROT_EXEC) ? -1 : 0;
}
