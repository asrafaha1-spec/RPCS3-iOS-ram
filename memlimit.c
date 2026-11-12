// memlimit.c - raises the iOS jetsam (memory) limit of the host process at launch.
//
// Built as a tiny dylib and injected into the RPCS3 iOS IPA (see patch_ipa.sh).
// It only works when the app is installed with TrollStore, because it needs the
// private entitlement com.apple.private.memorystatus (see entitlements-extra.plist).
//
// Default target: 3584 MB (3.5 GB). Override at build time with -DDEFAULT_LIMIT_MB=N,
// or at run time with the environment variable RPCS3_MEMLIMIT_MB.
//
// NOTE: the limit is only a ceiling set for the kernel's jetsam. The device still needs
// the physical RAM. On a 4 GB phone 3.5 GB will get killed by the system anyway.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syslog.h>
#include <pthread.h>
#include <time.h>
#include <mach/mach.h>
#include <mach/vm_region.h>

#if defined(__has_include)
#if __has_include(<os/proc.h>)
#include <os/proc.h>
#define HAVE_OS_PROC 1
#endif
#endif

#ifndef DEFAULT_LIMIT_MB
#define DEFAULT_LIMIT_MB 3584
#endif

// Private libsystem_kernel API (xnu bsd/sys/kern_memorystatus.h).
extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags,
                                void *buffer, size_t buffersize);

#define MEMORYSTATUS_CMD_SET_JETSAM_HIGH_WATER_MARK 5
#define MEMORYSTATUS_CMD_SET_JETSAM_TASK_LIMIT      6

// Log to syslog, stderr AND <app container>/Documents/memlimit.log (visible in the Files
// app under On My iPhone > RPCS3), because the app's own session log does not capture ours.
static void log_line(const char *line) {
    syslog(LOG_NOTICE, "[memlimit] %s", line);
    fprintf(stderr, "[memlimit] %s\n", line);

    const char *home = getenv("HOME");
    if (home) {
        char path[1024];
        snprintf(path, sizeof path, "%s/Documents/memlimit.log", home);
        FILE *f = fopen(path, "a");
        if (f) {
            fprintf(f, "[memlimit] %s\n", line);
            fclose(f);
        }
    }
}

static void log_msg(const char *fmt, int a, int b) {
    char line[160];
    snprintf(line, sizeof line, fmt, a, b);
    log_line(line);
}

// printf-style logger, also used by ramprobe.mm (same dylib, same log file).
void memlimit_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void memlimit_logf(const char *fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    log_line(line);
}

static long available_mb(void) {
#ifdef HAVE_OS_PROC
    if (&os_proc_available_memory != NULL)
        return (long)(os_proc_available_memory() / (1024 * 1024));
#endif
    return -1;
}


// Memory breakdown: walk this process' VM regions and sum dirty + compressed pages by kind.
// Shows what actually fills the footprint (malloc heap, file-backed mappings such as the PS3 shm
// file, other anonymous memory such as the JIT arena or GPU buffers) so we know whether lending
// memory from a helper process could help at all.
#define NB 6
static const char *bucket_name[NB] = {"malloc", "file-backed", "anon-untagged", "graphics/IOSurface", "stack", "other-tagged"};

static int bucket_of(const vm_region_submap_info_data_64_t *i) {
    unsigned t = i->user_tag;
    if (t >= 1 && t <= 11) return 0;          // VM_MEMORY_MALLOC*
    if (t == 30 || t == 31) return 4;         // stack
    if (t == 20 || t == 21 || t == 23 || t == 24 || t == 25 || t == 26 || t == 27) return 3; // GL/CG/IOSurface-ish
    if (i->external_pager) return 1;          // file-backed
    if (t == 0) return 2;
    return 5;
}

static void log_breakdown(int elapsed) {
    uint64_t sum[NB] = {0};
    uint64_t resident[NB] = {0};
    mach_vm_address_t addr = 0;
    natural_t depth = 0;
    const uint64_t page = vm_page_size;
    for (int guard = 0; guard < 200000; guard++) {
        vm_address_t a = (vm_address_t)addr;
        vm_size_t size = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t kr = vm_region_recurse_64(mach_task_self(), &a, &size, &depth,
                                                (vm_region_recurse_info_t)&info, &count);
        if (kr != KERN_SUCCESS) break;
        if (info.is_submap) { depth++; continue; }
        int b = bucket_of(&info);
        sum[b] += ((uint64_t)info.pages_dirtied + info.pages_swapped_out) * page;
        resident[b] += (uint64_t)info.pages_resident * page;
        addr = (mach_vm_address_t)a + size;
    }
    char line[400];
    int n = snprintf(line, sizeof line, "mem(t+%ds) dirty+swapped MB:", elapsed);
    for (int b = 0; b < NB && n < (int)sizeof line - 60; b++)
        n += snprintf(line + n, sizeof line - n, " %s=%llu(res %llu)", bucket_name[b],
                      (unsigned long long)(sum[b] >> 20), (unsigned long long)(resident[b] >> 20));
    log_line(line);
}

// Every 3 s, record how much memory this process may still allocate before iOS kills it.
// The last line before a crash shows how close to the limit the app was.
static void *monitor_thread(void *arg) {
    (void)arg;
    int elapsed = 0;
    for (;;) {
        sleep(3);
        elapsed += 3;
        log_msg("headroom=%dMB (t+%ds)", (int)available_mb(), elapsed);
        if (elapsed % 6 == 0)
            log_breakdown(elapsed);
#ifdef RAM_HELPER
        if (elapsed % 6 == 0) {
            extern void ramhelper_log_footprint(void);
            ramhelper_log_footprint();
        }
        if (elapsed % 30 == 0) {
            extern void ramhelper_log_stats(void);
            ramhelper_log_stats();
        }
#endif
    }
    return NULL;
}

// Public entry point: ask the kernel to raise this process' memory limit to `limit_mb`.
// Returns 0 on success, -1 if every attempt was refused.
int raise_memory_limit_mb(int limit_mb) {
    if (limit_mb <= 0)
        return -1;

    pid_t pid = getpid();
    long before = available_mb();

    // 1) Hard task limit (this is the one that matters for the "2.3 GB" ceiling).
    int r1 = memorystatus_control(MEMORYSTATUS_CMD_SET_JETSAM_TASK_LIMIT, pid,
                                  (uint32_t)limit_mb, NULL, 0);
    // 2) High-water mark, in case the task limit alone is refused on this iOS version.
    int r2 = memorystatus_control(MEMORYSTATUS_CMD_SET_JETSAM_HIGH_WATER_MARK, pid,
                                  (uint32_t)limit_mb, NULL, 0);

    long after = available_mb();
    log_msg("task_limit rc=%d, high_water rc=%d", r1, r2);
    log_msg("available before=%dMB after=%dMB", (int)before, (int)after);

    return (r1 == 0 || r2 == 0) ? 0 : -1;
}

__attribute__((constructor)) static void memlimit_init(void) {
    int limit = DEFAULT_LIMIT_MB;
    const char *env = getenv("RPCS3_MEMLIMIT_MB");
    if (env && *env) {
        int v = atoi(env);
        if (v > 0)
            limit = v;
    }
    int rc = raise_memory_limit_mb(limit);
    log_msg("requested %dMB, result=%d", limit, rc);

    pthread_t t;
    if (pthread_create(&t, NULL, monitor_thread, NULL) == 0)
        pthread_detach(t);

#ifdef RAM_HELPER
    // Experiment (see ramhelper/shmredirect.cpp): hooks the core's shm allocation to use RAM helper memory.
    // Does nothing unless Documents/ramhelper.enable exists.
    extern void ramhelper_install(void);
    ramhelper_install();
#endif

#ifdef RAM_PROBE
    // Experiment (see ramhelper/ramprobe.mm): only compiled in when the IPA is built with RAM_PROBE=1.
    extern void ramprobe_start(void);
    ramprobe_start();
#endif
}
