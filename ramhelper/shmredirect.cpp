// shmredirect.cpp - EXPERIMENTAL: back the emulator core's big guest-memory objects with RAM helper memory.
//
// The core (libRPCS3Core.dylib) creates every utils::shm like this (found by reading its disassembly):
//     fd = mkstemp(".../rpcs3-shm-XXXXXX"); unlink(path); ftruncate(fd, size);
// and maps it, always the whole object at offset 0, any number of times at fixed addresses:
//     mmap(addr, size, prot, MAP_SHARED|MAP_FIXED (0x11) or MAP_PRIVATE|MAP_FIXED (0x12), fd, 0)
// The guest address space views (vm::g_base_addr, g_sudo_addr, ...) are several mappings of one object.
//
// We rebind mkstemp/ftruncate/mmap/close in the core's own image only (fishhook). For a shm that is at
// least `threshold` bytes we ask the RAM helper for a region of that size and, instead of mapping the
// file, vm_remap() windows of the helper region into the requested addresses. All views of one shm
// share the same memory, exactly as with the file, but the pages are charged to the helper process.
// Everything else (small shm, private/COW mappings of non-helper fds, any failure) uses the original
// file path, decided once per shm before anything is mapped, so views never get out of sync.
//
// OFF by default: nothing is hooked unless Documents/ramhelper.enable exists. Its content may be
// "<threshold MiB> <cap MiB>" (defaults 8 and 1900).

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fishhook.h"
#include "ramhost.h"

extern "C" void memlimit_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define RLOG(...) memlimit_logf("[ramhost] " __VA_ARGS__)

extern "C" kern_return_t mach_vm_remap(vm_map_t target_task, mach_vm_address_t *target_address, mach_vm_size_t size,
                                       mach_vm_offset_t mask, int flags, vm_map_t src_task, mach_vm_address_t src_address,
                                       boolean_t copy, vm_prot_t *cur_protection, vm_prot_t *max_protection,
                                       vm_inherit_t inheritance);

namespace {

struct ShmInfo {
    bool used;   // an rpcs3-shm file
    bool helper; // backed by helper memory
    uint64_t size;
    void *base; // our mapping of the helper region (source of every view)
    uint64_t id;
};

const int kMaxFd = 16384;
ShmInfo g_fd[kMaxFd];
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

bool g_enabled = false; // cleared for good if the helper cannot be used
uint64_t g_threshold = 8ull << 20;
uint64_t g_cap = 1900ull << 20;
uint64_t g_lent = 0; // bytes currently lent (including reservations in flight)
unsigned g_redirected_shm = 0, g_fallback_shm = 0, g_redirected_maps = 0, g_failed_maps = 0, g_logged_maps = 0;

typedef int (*mkstemp_fn)(char *);
typedef int (*ftruncate_fn)(int, off_t);
typedef void *(*mmap_fn)(void *, size_t, int, int, int, off_t);
typedef int (*close_fn)(int);
typedef int (*mprotect_fn)(void *, size_t, int);
mkstemp_fn real_mkstemp;
ftruncate_fn real_ftruncate;
mmap_fn real_mmap;
close_fn real_close;
mprotect_fn real_mprotect;

unsigned long long mib(uint64_t bytes) { return (unsigned long long)(bytes >> 20); }

bool in_range(int fd) { return fd >= 0 && fd < kMaxFd; }

// Decide, once, whether this shm lives in the helper. Called right after ftruncate set its size.
void maybe_use_helper(int fd, uint64_t size) {
    pthread_mutex_lock(&g_lock);
    const bool enabled = g_enabled;
    const bool reserve_ok = g_lent + size <= g_cap;
    if (enabled && reserve_ok)
        g_lent += size; // reserve while we talk to the helper
    pthread_mutex_unlock(&g_lock);
    if (!enabled)
        return;
    if (!reserve_ok) {
        RLOG("shm fd=%d %llu MiB stays in a file: lent-memory cap reached", fd, mib(size));
        pthread_mutex_lock(&g_lock);
        g_fallback_shm++;
        pthread_mutex_unlock(&g_lock);
        return;
    }

    void *base = nullptr;
    uint64_t id = 0;
    bool ok = ramhost_start() && ramhost_create(size, &base, &id);

    pthread_mutex_lock(&g_lock);
    if (ok && g_fd[fd].used && !g_fd[fd].helper) {
        g_fd[fd].helper = true;
        g_fd[fd].size = size;
        g_fd[fd].base = base;
        g_fd[fd].id = id;
        g_redirected_shm++;
        const uint64_t lent = g_lent;
        pthread_mutex_unlock(&g_lock);
        RLOG("shm fd=%d %llu MiB -> helper region %llu (lent now %llu MiB)", fd, mib(size), (unsigned long long)id, mib(lent));
        return;
    }
    g_lent -= size;
    g_fallback_shm++;
    pthread_mutex_unlock(&g_lock);
    if (ok)
        ramhost_release(id, base, size);
    else
        RLOG("shm fd=%d %llu MiB stays in a file: helper unavailable", fd, mib(size));
}

int hook_mkstemp(char *tmpl) {
    const int fd = real_mkstemp(tmpl);
    if (fd >= 0 && in_range(fd) && tmpl && strstr(tmpl, "rpcs3-shm-")) {
        pthread_mutex_lock(&g_lock);
        g_fd[fd] = ShmInfo{ true, false, 0, nullptr, 0 };
        pthread_mutex_unlock(&g_lock);
    }
    return fd;
}

int hook_ftruncate(int fd, off_t length) {
    const int rc = real_ftruncate(fd, length);
    if (rc == 0 && in_range(fd) && length > 0) {
        pthread_mutex_lock(&g_lock);
        const ShmInfo info = g_fd[fd];
        pthread_mutex_unlock(&g_lock);
        if (info.used && !info.helper && (uint64_t)length >= g_threshold)
            maybe_use_helper(fd, (uint64_t)length);
    }
    return rc;
}

void *map_from_helper(const ShmInfo &info, void *addr, size_t length, int prot, int flags, off_t offset) {
    if (offset < 0 || (uint64_t)offset + length > info.size) {
        RLOG("mmap outside the object (off=%lld len=%zu size=%llu)", (long long)offset, length, mib(info.size));
        errno = EINVAL;
        return MAP_FAILED;
    }
    const bool fixed = (flags & MAP_FIXED) != 0;
    const bool priv = (flags & MAP_PRIVATE) != 0;
    mach_vm_address_t target = fixed ? (mach_vm_address_t)addr : 0;
    vm_prot_t cur = 0, max = 0;
    // Shared views alias the helper memory; a private view is a copy-on-write copy local to this app.
    const kern_return_t kr = mach_vm_remap(mach_task_self(), &target, length, 0,
                                           fixed ? (VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) : VM_FLAGS_ANYWHERE,
                                           mach_task_self(), (mach_vm_address_t)info.base + (mach_vm_address_t)offset,
                                           priv ? TRUE : FALSE, &cur, &max, priv ? VM_INHERIT_COPY : VM_INHERIT_SHARE);
    if (kr != KERN_SUCCESS) {
        pthread_mutex_lock(&g_lock);
        const unsigned n = ++g_failed_maps;
        pthread_mutex_unlock(&g_lock);
        if (n <= 20)
            RLOG("vm_remap failed (%s) addr=%p len=%zu prot=%d flags=0x%x", mach_error_string(kr), addr, length, prot, flags);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    if (prot != (PROT_READ | PROT_WRITE) && real_mprotect((void *)target, length, prot) != 0) {
        const int err = errno;
        pthread_mutex_lock(&g_lock);
        const unsigned n = ++g_failed_maps;
        pthread_mutex_unlock(&g_lock);
        if (n <= 20)
            RLOG("mprotect(prot=%d) on helper memory failed: errno=%d (cur=%d max=%d)", prot, err, cur, max);
        real_mprotect((void *)target, length, PROT_NONE);
        errno = EACCES;
        return MAP_FAILED;
    }
    pthread_mutex_lock(&g_lock);
    g_redirected_maps++;
    const unsigned n = ++g_logged_maps;
    pthread_mutex_unlock(&g_lock);
    if (n <= 24)
        RLOG("view %p len=%llu MiB prot=%d %s from region %llu", (void *)target, mib(length), prot, priv ? "private-copy" : "shared", (unsigned long long)info.id);
    return (void *)target;
}

void *hook_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (in_range(fd) && !(flags & MAP_ANON)) {
        pthread_mutex_lock(&g_lock);
        const ShmInfo info = g_fd[fd];
        pthread_mutex_unlock(&g_lock);
        if (info.used && info.helper)
            return map_from_helper(info, addr, length, prot, flags, offset);
    }
    return real_mmap(addr, length, prot, flags, fd, offset);
}

int hook_close(int fd) {
    if (in_range(fd)) {
        pthread_mutex_lock(&g_lock);
        const ShmInfo info = g_fd[fd];
        g_fd[fd] = ShmInfo{ false, false, 0, nullptr, 0 };
        if (info.helper)
            g_lent -= info.size;
        pthread_mutex_unlock(&g_lock);
        if (info.helper)
            ramhost_release(info.id, info.base, info.size);
    }
    return real_close(fd);
}

bool read_config() {
    const char *home = getenv("HOME");
    if (!home)
        return false;
    char path[1024];
    snprintf(path, sizeof path, "%s/Documents/ramhelper.enable", home);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    unsigned long long threshold = 0, cap = 0;
    const int n = fscanf(f, "%llu %llu", &threshold, &cap);
    fclose(f);
    if (n >= 1 && threshold > 0)
        g_threshold = threshold << 20;
    if (n >= 2 && cap > 0)
        g_cap = cap << 20;
    return true;
}

} // namespace

namespace {

// The app loads the core with dlopen() some time after launch, so it is not in the image list yet when
// our constructor runs. Poll for it, then hook its imports.
void *watch_for_core(void *) {
    for (int waited_ms = 0; waited_ms < 10 * 60 * 1000; waited_ms += 50) {
        for (uint32_t i = 0; i < _dyld_image_count(); i++) {
            const char *name = _dyld_get_image_name(i);
            if (!name || !strstr(name, "libRPCS3Core.dylib"))
                continue;
            struct rebinding hooks[] = {
                { "mkstemp", (void *)hook_mkstemp, nullptr },
                { "ftruncate", (void *)hook_ftruncate, nullptr },
                { "mmap", (void *)hook_mmap, nullptr },
                { "close", (void *)hook_close, nullptr },
            };
            const int rc = rebind_symbols_image((void *)_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i), hooks,
                                                sizeof hooks / sizeof hooks[0]);
            RLOG("hooked the core's mkstemp/ftruncate/mmap/close after %d ms (rc=%d)", waited_ms, rc);
            return nullptr;
        }
        usleep(50 * 1000);
    }
    RLOG("FAIL: libRPCS3Core.dylib never appeared, nothing hooked");
    pthread_mutex_lock(&g_lock);
    g_enabled = false;
    pthread_mutex_unlock(&g_lock);
    return nullptr;
}

} // namespace

extern "C" void ramhelper_log_stats(void) {
    if (!g_enabled)
        return;
    pthread_mutex_lock(&g_lock);
    RLOG("stats: shm in helper=%u (in files=%u), lent=%llu MiB of cap %llu MiB, views remapped=%u (failed %u)",
         g_redirected_shm, g_fallback_shm, mib(g_lent), mib(g_cap), g_redirected_maps, g_failed_maps);
    pthread_mutex_unlock(&g_lock);
}

extern "C" void ramhelper_install(void) {
    if (!read_config()) {
        RLOG("shm redirect is OFF (create Documents/ramhelper.enable in the app's Files folder to turn it on)");
        return;
    }
    real_mkstemp = (mkstemp_fn)dlsym(RTLD_DEFAULT, "mkstemp");
    real_ftruncate = (ftruncate_fn)dlsym(RTLD_DEFAULT, "ftruncate");
    real_mmap = (mmap_fn)dlsym(RTLD_DEFAULT, "mmap");
    real_close = (close_fn)dlsym(RTLD_DEFAULT, "close");
    real_mprotect = (mprotect_fn)dlsym(RTLD_DEFAULT, "mprotect");
    if (!real_mkstemp || !real_ftruncate || !real_mmap || !real_close || !real_mprotect) {
        RLOG("FAIL: could not resolve libc functions");
        return;
    }

    g_enabled = true;
    RLOG("shm redirect ON: threshold %llu MiB, cap %llu MiB; waiting for the app to dlopen libRPCS3Core.dylib",
         mib(g_threshold), mib(g_cap));
    pthread_t thread;
    if (pthread_create(&thread, nullptr, watch_for_core, nullptr) == 0) {
        pthread_detach(thread);
    } else {
        g_enabled = false;
        RLOG("FAIL: could not start the watcher thread, nothing hooked");
    }
}
