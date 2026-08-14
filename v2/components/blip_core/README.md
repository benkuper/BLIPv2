# blip_core

Host-testable lifecycle, descriptors, registry, scheduler, event bus, typed
errors, and core diagnostics under ADR-0001, ADR-0003, and ADR-0004.

The public headers are independent of ESP-IDF. Containers and queues have fixed
compile-time capacity; registry validation and descriptor serialization do not
allocate. ESP-IDF integration is limited to the component build file.
