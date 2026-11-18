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
#include <os/proc.h>
#include <atomic>
#include <time.h>
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
unsigned g_kill_after_s = 0;   // MeloCafe style: kill the helper this long after the last mapping (0 = keep it alive)
unsigned g_kill_gen = 0;       // bumped by every redirect; a pending kill is dropped when it is stale
unsigned g_inflight = 0;       // helper requests being made right now
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

void log_foot_now(const char *label);

// Kill mode (Documents/ramhelper.kill), as in MeloCafe's killDonorsAfterMapping: once the lent memory
// is mapped the helper is killed, so the memory is owned by no process and is charged to none. The core
// allocates in stages, so the kill waits until nothing new has been lent for a few seconds; the next big
// object starts a fresh helper. Logs both footprints around the kill.
void *kill_thread(void *arg) {
    const unsigned generation = (unsigned)(uintptr_t)arg;
    sleep(g_kill_after_s);
    pthread_mutex_lock(&g_lock);
    const bool stale = generation != g_kill_gen || g_inflight > 0;
    pthread_mutex_unlock(&g_lock);
    if (stale)
        return nullptr;
    log_foot_now("before kill");
    ramhost_kill();
    sleep(2);
    log_foot_now("2 s after kill");
    sleep(8);
    log_foot_now("10 s after kill");
    return nullptr;
}

void schedule_kill() {
    if (!g_kill_after_s)
        return;
    pthread_mutex_lock(&g_lock);
    const unsigned generation = ++g_kill_gen;
    pthread_mutex_unlock(&g_lock);
    pthread_t thread;
    if (pthread_create(&thread, nullptr, kill_thread, (void *)(uintptr_t)generation) == 0)
        pthread_detach(thread);
}

// Decide, once, whether this shm lives in the helper. Called right after ftruncate set its size.
void maybe_use_helper(int fd, uint64_t size) {
    pthread_mutex_lock(&g_lock);
    const bool enabled = g_enabled;
    const bool reserve_ok = g_lent + size <= g_cap;
    if (enabled && reserve_ok) {
        g_lent += size; // reserve while we talk to the helper
        g_inflight++;
        g_kill_gen++; // a pending kill must wait for this one
    }
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
        g_inflight--;
        const uint64_t lent = g_lent;
        pthread_mutex_unlock(&g_lock);
        schedule_kill();
        RLOG("shm fd=%d %llu MiB -> helper region %llu (lent now %llu MiB)", fd, mib(size), (unsigned long long)id, mib(lent));
        return;
    }
    g_lent -= size;
    g_inflight--;
    g_fallback_shm++;
    pthread_mutex_unlock(&g_lock);
    schedule_kill();
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

// ---- Soft memory budget for the core ----------------------------------------------------------------
// The core already trims its texture/surface caches when memory runs short: its severity function reads
// rpcs3::ios::available_process_memory_headroom(), which is a thin wrapper around the imported
// os_proc_available_memory(), and maps it to levels (<1.5 GiB: 1, <1 GiB: 2, <512 MiB: 3). With the kernel
// limit raised to 3.5 GB that headroom stays large, so the trimming never starts. We rebind
// os_proc_available_memory in the core only and report min(real, budget - footprint - lent memory in use),
// so the core's own LRU trimming starts at the budget we choose while the kernel limit stays high.
// Documents/ramhelper.budget holds the budget in MiB (default 3072, 0 = off).

typedef size_t (*oapm_fn)(void);
oapm_fn real_oapm = nullptr;
uint64_t g_budget = 3072ull << 20;
bool g_budget_on = true;
std::atomic<int> g_last_level{ -1 };
std::atomic<unsigned> g_level_logs{ 0 };
std::atomic<uint64_t> g_oapm_calls{ 0 }, g_last_soft{ 0 }, g_last_real{ 0 }, g_last_lent{ 0 };

// Pages of helper-backed shm objects that are really in use. They are charged to the helper (or to
// nobody after a kill), not to this app, but they still take physical RAM. Measured on the base mapping we
// keep of every region; cached for one second.
uint64_t lent_touched_bytes() {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t cached = 0;
    static time_t cached_at = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&m);
    if (cached_at != 0 && now.tv_sec - cached_at < 1) {
        const uint64_t value = cached;
        pthread_mutex_unlock(&m);
        return value;
    }
    pthread_mutex_unlock(&m);

    struct Span {
        uintptr_t base;
        uint64_t size;
    };
    Span spans[256];
    int n = 0;
    pthread_mutex_lock(&g_lock);
    for (int fd = 0; fd < kMaxFd && n < 256; fd++)
        if (g_fd[fd].used && g_fd[fd].helper)
            spans[n++] = { (uintptr_t)g_fd[fd].base, g_fd[fd].size };
    pthread_mutex_unlock(&g_lock);

    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        uint64_t resident = 0;
        vm_address_t a = spans[i].base;
        const vm_address_t end = spans[i].base + spans[i].size;
        for (int guard = 0; a < end && guard < 64; guard++) {
            vm_size_t sz = 0;
            vm_region_extended_info_data_t info;
            mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
            mach_port_t object = MACH_PORT_NULL;
            if (vm_region_64(mach_task_self(), &a, &sz, VM_REGION_EXTENDED_INFO, (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS || sz == 0)
                break;
            resident += (uint64_t)info.pages_resident * vm_page_size;
            a += sz;
        }
        total += resident < spans[i].size ? resident : spans[i].size;
    }
    pthread_mutex_lock(&m);
    cached = total;
    cached_at = now.tv_sec ? now.tv_sec : 1;
    pthread_mutex_unlock(&m);
    return total;
}

size_t hook_os_proc_available_memory(void) {
    const size_t real = real_oapm();
    if (!g_budget_on)
        return real;
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
        return real;
    const uint64_t lent = lent_touched_bytes();
    const uint64_t used = (uint64_t)info.phys_footprint + lent;
    const uint64_t soft = g_budget > used ? g_budget - used : 0;
    const size_t out = soft < real ? (size_t)soft : real;

    g_oapm_calls++;
    g_last_soft = soft;
    g_last_real = real;
    g_last_lent = lent;
    const int level = out < (512ull << 20) ? 3 : out < (1ull << 30) ? 2 : out < (1536ull << 20) ? 1 : 0;
    const int previous = g_last_level.exchange(level);
    if (previous != level && g_level_logs++ < 200)
        RLOG("pressure level the core sees: %d -> %d (reported headroom %llu MiB, real %llu MiB, footprint %llu MiB, lent in use %llu MiB)",
             previous, level, mib(out), mib(real), mib(info.phys_footprint), mib(lent));
    return out;
}

bool read_budget() {
    const char *home = getenv("HOME");
    if (!home)
        return true;
    char path[1024];
    snprintf(path, sizeof path, "%s/Documents/ramhelper.budget", home);
    FILE *f = fopen(path, "r");
    if (!f)
        return true; // default on
    unsigned long long value = 0;
    const int n = fscanf(f, "%llu", &value);
    fclose(f);
    if (n == 1) {
        if (value == 0)
            return false;
        g_budget = value << 20;
    }
    return true;
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
    {
        char kill_path[1024];
        snprintf(kill_path, sizeof kill_path, "%s/Documents/ramhelper.kill", home);
        if (access(kill_path, F_OK) == 0)
            g_kill_after_s = 2;
    }
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
            struct rebinding hooks[5];
            int count = 0;
            if (g_enabled) {
                hooks[count++] = { "mkstemp", (void *)hook_mkstemp, nullptr };
                hooks[count++] = { "ftruncate", (void *)hook_ftruncate, nullptr };
                hooks[count++] = { "mmap", (void *)hook_mmap, nullptr };
                hooks[count++] = { "close", (void *)hook_close, nullptr };
            }
            if (g_budget_on)
                hooks[count++] = { "os_proc_available_memory", (void *)hook_os_proc_available_memory, nullptr };
            const int rc = rebind_symbols_image((void *)_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i), hooks, count);
            RLOG("hooked %d imports of the core after %d ms (shm redirect %s, soft budget %s) rc=%d", count, waited_ms,
                 g_enabled ? "on" : "off", g_budget_on ? "on" : "off", rc);
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

// The decisive numbers: is the lent memory charged to this app or to the helper?
namespace {
void log_foot_now(const char *label) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    const uint64_t app = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS ? info.phys_footprint : 0;
    pthread_mutex_lock(&g_lock);
    const uint64_t lent = g_lent;
    pthread_mutex_unlock(&g_lock);
    const uint64_t helper = ramhost_helper_footprint();
    RLOG("foot[%s]: app=%llu MiB (compressed %llu), available=%llu MiB, helper=%llu MiB, lent(mapped)=%llu MiB", label,
         mib(app), mib(info.compressed), mib(os_proc_available_memory()), mib(helper), mib(lent));
}
} // namespace

extern "C" void ramhelper_log_footprint(void) {
    if (!g_enabled)
        return;
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    const uint64_t app = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS ? info.phys_footprint : 0;
    pthread_mutex_lock(&g_lock);
    const uint64_t lent = g_lent;
    pthread_mutex_unlock(&g_lock);
    if (lent == 0)
        return;
    const uint64_t helper = ramhost_helper_footprint();
    RLOG("foot: app=%llu MiB (compressed %llu), helper=%llu MiB, lent(mapped)=%llu MiB", mib(app), mib(info.compressed), mib(helper), mib(lent));
}

extern "C" void ramhelper_log_stats(void) {
    if (g_budget_on && g_oapm_calls > 0)
        RLOG("budget: core asked for headroom %llu times; last reported %llu MiB (real %llu MiB), lent in use %llu MiB",
             (unsigned long long)g_oapm_calls.load(), mib(g_last_soft), mib(g_last_real), mib(g_last_lent));
    if (!g_enabled)
        return;
    pthread_mutex_lock(&g_lock);
    RLOG("stats: shm in helper=%u (in files=%u), lent=%llu MiB of cap %llu MiB, views remapped=%u (failed %u)",
         g_redirected_shm, g_fallback_shm, mib(g_lent), mib(g_cap), g_redirected_maps, g_failed_maps);
    pthread_mutex_unlock(&g_lock);
}

extern "C" void ramhelper_install(void) {
    g_budget_on = read_budget();
    const bool shm_on = read_config();
    real_oapm = (oapm_fn)dlsym(RTLD_DEFAULT, "os_proc_available_memory");
    if (!real_oapm)
        g_budget_on = false;
    if (!shm_on)
        RLOG("shm redirect is OFF (create Documents/ramhelper.enable in the app's Files folder to turn it on)");
    if (g_budget_on)
        RLOG("soft memory budget ON: %llu MiB (Documents/ramhelper.budget changes it, 0 turns it off)", mib(g_budget));
    else
        RLOG("soft memory budget is OFF");
    if (!shm_on && !g_budget_on)
        return;

    if (shm_on) {
        real_mkstemp = (mkstemp_fn)dlsym(RTLD_DEFAULT, "mkstemp");
        real_ftruncate = (ftruncate_fn)dlsym(RTLD_DEFAULT, "ftruncate");
        real_mmap = (mmap_fn)dlsym(RTLD_DEFAULT, "mmap");
        real_close = (close_fn)dlsym(RTLD_DEFAULT, "close");
        real_mprotect = (mprotect_fn)dlsym(RTLD_DEFAULT, "mprotect");
        if (!real_mkstemp || !real_ftruncate || !real_mmap || !real_close || !real_mprotect) {
            RLOG("FAIL: could not resolve libc functions; shm redirect stays off");
        } else {
            g_enabled = true;
            RLOG("shm redirect ON: threshold %llu MiB, cap %llu MiB", mib(g_threshold), mib(g_cap));
        }
    }
    if (!g_enabled && !g_budget_on)
        return;

    pthread_t thread;
    if (pthread_create(&thread, nullptr, watch_for_core, nullptr) == 0) {
        pthread_detach(thread);
    } else {
        g_enabled = false;
        g_budget_on = false;
        RLOG("FAIL: could not start the watcher thread, nothing hooked");
    }
}
