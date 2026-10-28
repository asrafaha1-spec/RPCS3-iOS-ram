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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syslog.h>

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

static void log_msg(const char *fmt, int a, int b) {
    char line[160];
    snprintf(line, sizeof line, fmt, a, b);
    syslog(LOG_NOTICE, "[memlimit] %s", line);
    fprintf(stderr, "[memlimit] %s\n", line);
}

static long available_mb(void) {
#ifdef HAVE_OS_PROC
    if (&os_proc_available_memory != NULL)
        return (long)(os_proc_available_memory() / (1024 * 1024));
#endif
    return -1;
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
}
