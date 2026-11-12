// ramhost.mm - host-app client of the RamHelper.appex extension.
//
// Starts the extension through the private NSExtension API, connects to it over an xpc endpoint, and
// asks it for regions of memory. Region memory is allocated (purgeable) by the helper process, so it
// is charged to the helper's own memory limit and not to this app's, but this process can map it with
// xpc_shmem_map and use it like ordinary memory.
//
// Adapted from Vita3KiOS ram_helpers.mm (oledz-i/Vita3KiOS, GPL-2.0-or-later); the "RAM donor"
// technique is by MeloNX (Stossy11, MIT). Same code as the probe in ramprobe.mm, as a small library.

#import <Foundation/Foundation.h>
#import <dlfcn.h>
#import <mach/mach.h>
#import <os/proc.h>
#import <xpc/xpc.h>

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>

#import "protocol.h"
#import "ramhost.h"

extern "C" void memlimit_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define HLOG(...) memlimit_logf("[ramhost] " __VA_ARGS__)

@interface NSXPCListenerEndpoint (RamHost)
@property (nonatomic, setter=_setEndpoint:) xpc_object_t _endpoint;
@end

@protocol RamHostExtensionAPI <NSObject>
@property (nonatomic, strong, readwrite) NSArray *preferredLanguages;
+ (id<RamHostExtensionAPI>)extensionWithIdentifier:(NSString *)identifier error:(NSError **)error;
- (void)beginExtensionRequestWithInputItems:(NSArray *)items completion:(void (^)(NSUUID *))callback;
- (void)_kill:(int)signal;
- (void)setRequestCancellationBlock:(void (^)(NSUUID *uuid, NSError *error))callback;
@end

extern "C" kern_return_t mach_vm_deallocate(vm_map_t target, mach_vm_address_t address, mach_vm_size_t size);

namespace {

const double kStartTimeout = 10.0;
const int kMaxStartAttempts = 3;

pthread_mutex_t g_start_lock = PTHREAD_MUTEX_INITIALIZER;
volatile bool g_connected_flag = false;
int g_attempts = 0;

xpc_connection_t g_listener = nil;
xpc_connection_t g_helper = nil;
id<RamHostExtensionAPI> g_extension = nil;

NSString *helper_identifier() {
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

xpc_object_t send_op(const char *op, uint64_t size, uint64_t id, double timeout) {
    xpc_connection_t helper = g_helper;
    if (!helper)
        return nil;
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(request, RAMH_KEY_OP, op);
    if (size)
        xpc_dictionary_set_uint64(request, RAMH_KEY_SIZE, size);
    if (id)
        xpc_dictionary_set_uint64(request, RAMH_KEY_ID, id);
    __block xpc_object_t answer = nil;
    dispatch_semaphore_t replied = dispatch_semaphore_create(0);
    xpc_connection_send_message_with_reply(helper, request, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(xpc_object_t reply) {
        if (xpc_get_type(reply) == XPC_TYPE_DICTIONARY)
            answer = reply;
        dispatch_semaphore_signal(replied);
    });
    if (dispatch_semaphore_wait(replied, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout * NSEC_PER_SEC))) != 0)
        return nil;
    return answer;
}

bool start_once() {
    NSString *identifier = helper_identifier();
    if (!identifier) {
        HLOG("FAIL: no RamHelper.appex in PlugIns (the installer may have removed it)");
        return false;
    }

    dispatch_semaphore_t connected = dispatch_semaphore_create(0);
    g_listener = xpc_connection_create(NULL, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    xpc_connection_set_event_handler(g_listener, ^(xpc_object_t event) {
        if (xpc_get_type(event) != XPC_TYPE_CONNECTION)
            return;
        xpc_connection_t connection = (xpc_connection_t)event;
        xpc_connection_set_event_handler(connection, ^(xpc_object_t message) {
            if (xpc_get_type(message) == XPC_TYPE_ERROR) {
                g_connected_flag = false;
                HLOG("helper connection closed (memory already mapped stays valid)");
            }
        });
        xpc_connection_resume(connection);
        g_helper = connection;
        dispatch_semaphore_signal(connected);
    });
    xpc_connection_resume(g_listener);

    Class extension_class = NSClassFromString(@"NSExtension");
    if (!extension_class) {
        dlopen("/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation", RTLD_LAZY | RTLD_GLOBAL);
        extension_class = NSClassFromString(@"NSExtension");
    }
    if (!extension_class) {
        HLOG("FAIL: this iOS has no NSExtension class");
        return false;
    }
    NSError *error = nil;
    id<RamHostExtensionAPI> extension = [(Class<RamHostExtensionAPI>)extension_class extensionWithIdentifier:identifier error:&error];
    if (!extension) {
        HLOG("FAIL: extensionWithIdentifier: %s", error.localizedDescription.UTF8String ?: "unknown error");
        return false;
    }
    extension.preferredLanguages = @[];
    [extension setRequestCancellationBlock:^(NSUUID *, NSError *cancel_error) {
        g_connected_flag = false;
        HLOG("helper request cancelled: %s", cancel_error.localizedDescription.UTF8String ?: "");
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
        HLOG("FAIL: iOS did not start the helper process");
        return false;
    }
    if (dispatch_semaphore_wait(connected, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kStartTimeout * NSEC_PER_SEC))) != 0) {
        HLOG("FAIL: helper started but never connected");
        [extension _kill:SIGKILL];
        return false;
    }
    g_extension = extension;
    g_connected_flag = true;
    return true;
}

} // namespace

extern "C" bool ramhost_start(void) {
    if (g_connected_flag)
        return true;
    pthread_mutex_lock(&g_start_lock);
    bool ok = g_connected_flag;
    if (!ok && g_attempts < kMaxStartAttempts) {
        g_attempts++;
        @autoreleasepool {
            ok = start_once();
        }
        HLOG("helper start attempt %d: %s", g_attempts, ok ? "connected" : "failed");
    }
    pthread_mutex_unlock(&g_start_lock);
    return ok;
}

extern "C" uint64_t ramhost_helper_available(void) {
    xpc_object_t reply = send_op(RAMH_OP_AVAILABLE, 0, 0, 3.0);
    return reply ? xpc_dictionary_get_uint64(reply, RAMH_KEY_AVAILABLE) : 0;
}

extern "C" bool ramhost_create(uint64_t size, void **base, uint64_t *id) {
    if (!g_connected_flag)
        return false;
    xpc_object_t reply = send_op(RAMH_OP_CREATE, size, 0, 20.0);
    if (!reply) {
        HLOG("create %llu MiB: helper did not answer", (unsigned long long)(size >> 20));
        return false;
    }
    if (const char *err = xpc_dictionary_get_string(reply, RAMH_KEY_ERROR)) {
        HLOG("create %llu MiB: helper refused: %s", (unsigned long long)(size >> 20), err);
        return false;
    }
    xpc_object_t shmem = xpc_dictionary_get_value(reply, RAMH_KEY_SHMEM);
    if (!shmem || xpc_get_type(shmem) != XPC_TYPE_SHMEM) {
        HLOG("create: reply had no shared memory");
        return false;
    }
    void *region = nullptr;
    const size_t mapped = xpc_shmem_map(shmem, &region);
    if (!region || mapped < size) {
        HLOG("create %llu MiB: could not map the shared memory", (unsigned long long)(size >> 20));
        if (region)
            mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)region, mapped);
        return false;
    }
    *base = region;
    *id = xpc_dictionary_get_uint64(reply, RAMH_KEY_ID);
    return true;
}

extern "C" void ramhost_release(uint64_t id, void *base, uint64_t size) {
    mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)base, size);
    xpc_connection_t helper = g_helper;
    if (helper && g_connected_flag && id) {
        xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_string(request, RAMH_KEY_OP, RAMH_OP_RELEASE);
        xpc_dictionary_set_uint64(request, RAMH_KEY_ID, id);
        xpc_connection_send_message(helper, request);
    }
}
