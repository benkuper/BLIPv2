/* The interpreter's linear mapping is part of the declared WAMR pool. */
#include "platform_api_vmcore.h"
#include "wasm_export.h"

void *os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
    (void)flags;
    (void)file;
    if (hint || (prot & MMAP_PROT_EXEC) || size > 65536) return NULL;
    void *memory = wasm_runtime_malloc((unsigned int)size);
    if (memory) memset(memory, 0, size);
    return memory;
}
void *os_mremap(void *old_addr, size_t old_size, size_t new_size)
{
    if (new_size > 65536) return NULL;
    void *memory = wasm_runtime_realloc(old_addr, (unsigned int)new_size);
    if (memory && new_size > old_size) memset((uint8_t *)memory + old_size, 0, new_size - old_size);
    return memory;
}
void os_munmap(void *addr, size_t size) { (void)size; wasm_runtime_free(addr); }
int os_mprotect(void *addr, size_t size, int prot)
{
    (void)addr;
    (void)size;
    return (prot & MMAP_PROT_EXEC) ? -1 : 0;
}
