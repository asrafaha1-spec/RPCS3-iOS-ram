// ramhost.h - host-app client of the RamHelper.appex extension (see ramhost.mm).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Starts the helper process once and waits until it has connected. Safe to call from any thread and
// as often as you like; returns true while a helper is connected.
bool ramhost_start(void);

// os_proc_available_memory() of the helper process, in bytes (0 when unknown).
uint64_t ramhost_helper_available(void);

// phys_footprint of the helper process, in bytes (0 when unknown).
uint64_t ramhost_helper_footprint(void);

// Allocates `size` bytes in the helper (purgeable, charged to the helper) and maps them here.
// On success *base is the mapping in this process and *id names the region for ramhost_release().
bool ramhost_create(uint64_t size, void **base, uint64_t *id);

// Unmaps the region from this process and tells the helper to drop it. Views made from it with
// vm_remap keep the memory alive until they are unmapped too.
void ramhost_release(uint64_t id, void *base, uint64_t size);

#ifdef __cplusplus
}
#endif
