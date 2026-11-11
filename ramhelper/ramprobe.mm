// ramprobe.mm - EXPERIMENT: does a RAM helper extension work on this device, and does the memory it
// lends count against the app's own memory limit?
//
// Compiled into libmemlimit.dylib only when the IPA is built with RAM_PROBE=1. It does NOT hook the
// emulator core, so the lent memory is never used by the game. It only measures, and writes the
// results to Documents/memlimit.log (Files app > On My iPhone > RPCS3).
//
// What it does, ~12 s after launch:
//   1. starts the RamHelper.appex extension (its own process, its own memory limit)
//   2. asks the helper for its own available memory
//   3. borrows N chunks of 256 MiB (default 4, env RPCS3_RAMPROBE_CHUNKS), maps and touches every page,
//      and logs this app's available memory and footprint after each one
//   4. kills the helper and logs again (the mapping stays; who is charged now?)
//   5. unmaps everything and logs a last time
//
// Host side adapted from Vita3KiOS ram_helpers.mm (oledz-i/Vita3KiOS, GPL-2.0-or-later);
// "RAM donor" technique by MeloNX (Stossy11, MIT).

#import <Foundation/Foundation.h>
#import <dlfcn.h>
#import <mach/mach.h>
#import <os/proc.h>
#import <xpc/xpc.h>

#include <signal.h>
#include <stdarg.h>
#include <unistd.h>

#import "protocol.h"

extern "C" void memlimit_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define PLOG(...) memlimit_logf("[ramprobe] " __VA_ARGS__)

// The xpc endpoint travels inside an NSXPCListenerEndpoint (the only endpoint NSExtensionItem can
// carry); its private _endpoint property holds the raw xpc object.
@interface NSXPCListenerEndpoint (RamProbe)
@property (nonatomic, setter=_setEndpoint:) xpc_object_t _endpoint;
@end

// Private NSExtension API: how an app starts its own extensions. Looked up at run time.
@protocol RamProbeExtensionAPI <NSObject>
@property (nonatomic, strong, readwrite) NSArray *preferredLanguages;
+ (id<RamProbeExtensionAPI>)extensionWithIdentifier:(NSString *)identifier error:(NSError **)error;
- (void)beginExtensionRequestWithInputItems:(NSArray *)items completion:(void (^)(NSUUID *))callback;
- (void)_kill:(int)signal;
- (void)setRequestCancellationBlock:(void (^)(NSUUID *uuid, NSError *error))callback;
@end

extern "C" kern_return_t mach_vm_deallocate(vm_map_t target, mach_vm_address_t address, mach_vm_size_t size);

static const double kStartTimeout = 8.0;
static const unsigned kDelaySeconds = 12;
static const int kMaxChunks = 12;

static xpc_connection_t g_listener = nil;
static xpc_connection_t g_helper = nil;
static dispatch_semaphore_t g_connected = nil;

static unsigned long long mib(uint64_t bytes) { return (unsigned long long)(bytes >> 20); }

static uint64_t app_footprint(void) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
        return 0;
    return info.phys_footprint;
}

static void snap(const char *label) {
    PLOG("%-26s app available=%llu MiB, footprint=%llu MiB", label, mib(os_proc_available_memory()), mib(app_footprint()));
}

static NSString *helper_identifier(void) {
    NSArray<NSURL *> *contents = [NSFileManager.defaultManager contentsOfDirectoryAtURL:NSBundle.mainBundle.builtInPlugInsURL
                                                             includingPropertiesForKeys:nil
                                                                                options:0
                                                                                  error:nil];
    for (NSURL *url in contents) {
        if (![url.pathExtension isEqualToString:@"appex"])
            continue;
        NSBundle *bundle = [NSBundle bundleWithURL:url];
        if ([bundle.infoDictionary[@"NSExtension"][@"NSExtensionPrincipalClass"] isEqualToString:@RAMH_HELPER_PRINCIPAL])
            return bundle.bundleIdentifier;
    }
    return nil;
}

static xpc_object_t send_op(const char *op, uint64_t size, double timeout) {
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(request, RAMH_KEY_OP, op);
    if (size)
        xpc_dictionary_set_uint64(request, RAMH_KEY_SIZE, size);
    __block xpc_object_t answer = nil;
    dispatch_semaphore_t replied = dispatch_semaphore_create(0);
    xpc_connection_send_message_with_reply(g_helper, request, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(xpc_object_t reply) {
        if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY)
            answer = reply;
        dispatch_semaphore_signal(replied);
    });
    if (dispatch_semaphore_wait(replied, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout * NSEC_PER_SEC))) != 0)
        return nil;
    return answer;
}

static void run_probe(void) {
    PLOG("=== RAM helper probe start (iOS %s) ===", NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);
    snap("before");

    NSString *identifier = helper_identifier();
    if (!identifier) {
        PLOG("FAIL: no RamHelper.appex in PlugIns (the installer may have removed it)");
        return;
    }
    PLOG("helper bundle id: %s", identifier.UTF8String);

    // Listener the helper connects back to.
    g_connected = dispatch_semaphore_create(0);
    g_listener = xpc_connection_create(NULL, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    xpc_connection_set_event_handler(g_listener, ^(xpc_object_t event) {
        if (xpc_get_type(event) != XPC_TYPE_CONNECTION)
            return;
        xpc_connection_t connection = (xpc_connection_t)event;
        xpc_connection_set_event_handler(connection, ^(xpc_object_t message) {
            if (xpc_get_type(message) == XPC_TYPE_ERROR)
                PLOG("helper connection closed");
        });
        xpc_connection_resume(connection);
        g_helper = connection;
        dispatch_semaphore_signal(g_connected);
    });
    xpc_connection_resume(g_listener);

    Class extension_class = NSClassFromString(@"NSExtension");
    if (!extension_class) {
        dlopen("/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation", RTLD_LAZY | RTLD_GLOBAL);
        extension_class = NSClassFromString(@"NSExtension");
    }
    if (!extension_class) {
        PLOG("FAIL: this iOS has no NSExtension class");
        return;
    }
    NSError *error = nil;
    id<RamProbeExtensionAPI> extension = [(Class<RamProbeExtensionAPI>)extension_class extensionWithIdentifier:identifier error:&error];
    if (!extension) {
        PLOG("FAIL: extensionWithIdentifier: %s", error.localizedDescription.UTF8String ?: "unknown error");
        return;
    }
    extension.preferredLanguages = @[];
    [extension setRequestCancellationBlock:^(NSUUID *, NSError *cancel_error) {
        PLOG("helper request cancelled: %s", cancel_error.localizedDescription.UTF8String ?: "");
    }];

    NSXPCListenerEndpoint *wrapper = [NSXPCListenerEndpoint new];
    wrapper._endpoint = xpc_endpoint_create(g_listener);
    NSExtensionItem *item = [NSExtensionItem new];
    item.userInfo = @{ @"endpoint" : wrapper };

    __block bool started = false;
    dispatch_semaphore_t spawned = dispatch_semaphore_create(0);
    [extension beginExtensionRequestWithInputItems:@[ item ]
                                        completion:^(NSUUID *request) {
                                            started = request != nil;
                                            dispatch_semaphore_signal(spawned);
                                        }];
    dispatch_semaphore_wait(spawned, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kStartTimeout * NSEC_PER_SEC)));
    if (!started) {
        PLOG("FAIL: iOS did not start the helper process");
        return;
    }
    if (dispatch_semaphore_wait(g_connected, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kStartTimeout * NSEC_PER_SEC))) != 0) {
        PLOG("FAIL: helper started but never connected (endpoint probably decoded as nil)");
        [extension _kill:SIGKILL];
        return;
    }
    PLOG("helper is running and connected");

    xpc_object_t reply = send_op(RAMH_OP_AVAILABLE, 0, 3.0);
    const uint64_t helper_available = reply ? xpc_dictionary_get_uint64(reply, RAMH_KEY_AVAILABLE) : 0;
    PLOG("helper's own available memory: %llu MiB", mib(helper_available));

    int wanted = 4;
    if (const char *env = getenv("RPCS3_RAMPROBE_CHUNKS"))
        if (atoi(env) > 0)
            wanted = atoi(env);
    if (wanted > kMaxChunks)
        wanted = kMaxChunks;

    void *mappings[kMaxChunks] = {};
    size_t sizes[kMaxChunks] = {};
    NSMutableArray *shmems = [NSMutableArray array];
    int got = 0;
    for (int i = 0; i < wanted; i++) {
        reply = send_op(RAMH_OP_CREATE, RAMH_HELPER_CHUNK, 20.0);
        if (!reply) {
            PLOG("chunk %d: helper did not answer", i + 1);
            break;
        }
        if (const char *err = xpc_dictionary_get_string(reply, RAMH_KEY_ERROR)) {
            PLOG("chunk %d: helper refused: %s", i + 1, err);
            break;
        }
        xpc_object_t shmem = xpc_dictionary_get_value(reply, RAMH_KEY_SHMEM);
        if (!shmem || xpc_get_type(shmem) != XPC_TYPE_SHMEM) {
            PLOG("chunk %d: reply had no shared memory", i + 1);
            break;
        }
        void *region = nullptr;
        const size_t mapped = xpc_shmem_map(shmem, &region);
        if (!region || mapped < RAMH_HELPER_CHUNK) {
            PLOG("chunk %d: could not map the shared memory", i + 1);
            break;
        }
        [shmems addObject:shmem];
        mappings[got] = region;
        sizes[got] = mapped;
        got++;
        // Touch every page so the memory is really resident, then see who gets charged.
        const size_t page = vm_page_size;
        for (size_t off = 0; off < mapped; off += page)
            static_cast<volatile char *>(region)[off] = 1;
        char label[40];
        snprintf(label, sizeof label, "after chunk %d (+%llu MiB)", got, mib((uint64_t)got * RAMH_HELPER_CHUNK));
        snap(label);
        xpc_object_t fp = send_op(RAMH_OP_FOOTPRINT, 0, 3.0);
        PLOG("  helper footprint=%llu MiB", fp ? mib(xpc_dictionary_get_uint64(fp, RAMH_KEY_FOOTPRINT)) : 0ull);
    }
    PLOG("borrowed %d of %d chunks", got, wanted);

    [extension _kill:SIGKILL];
    sleep(3);
    snap("helper killed, still mapped");
    sleep(10);
    snap("10 s later");

    for (int i = 0; i < got; i++)
        mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)mappings[i], sizes[i]);
    snap("after unmap");
    PLOG("=== probe done ===");
}

extern "C" void ramprobe_start(void) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        sleep(kDelaySeconds);
        @autoreleasepool {
            run_probe();
        }
    });
}
