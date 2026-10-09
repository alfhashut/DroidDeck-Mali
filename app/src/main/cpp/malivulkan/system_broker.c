/* Opt-in Android system Vulkan query/lifecycle/memory diagnostic broker. No normal renderer or driver override. */
#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <android/hardware_buffer.h>
#include <poll.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include "protocol.h"
#include "capabilities.h"
#include "submit_protocol.h"
#include "submit_objects.h"
#include "interop_protocol.h"
#include "interop_objects.h"
#include "renderer_protocol.h"
#include "renderer_objects.h"
#include "interop_consumer.h"

static _Thread_local int session_quiet;
#define LOG(...) do { if (!session_quiet) __android_log_print(ANDROID_LOG_INFO, "MaliVulkanBroker", __VA_ARGS__); } while (0)
#define ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "MaliVulkanBroker", __VA_ARGS__)
#define MAX_CLIENTS 16

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static int listener = -1, running, stopping;
static char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static struct client_slot {
    int fd, started, done;
    pthread_t worker;
} clients[MAX_CLIENTS];

/* All Android handles live here, owned by one connection. Wire IDs are indices. */
struct vk_session {
    struct vk_session *quarantined_next;
    void *library;
    VkInstance instance;
    PFN_vkDestroyInstance destroy;
    PFN_vkEnumeratePhysicalDevices enumerate;
    PFN_vkGetPhysicalDeviceProperties properties;
    PFN_vkGetInstanceProcAddr gipa;
    uint32_t native_api, enabled_queries;
    VkPhysicalDevice devices[MB_MAX_DEVICES];
    uint32_t count;
    int listed;
    uint32_t wire_version, next_device_id, next_queue_id, next_resource_id;
    struct native_device {
        struct {
            struct mb_consumer_session *consumer;
            uint32_t enabled, held, presented, released, fds_created, fds_closed, max_owned;
            uint32_t last_sync, last_fence, release_timeouts, cleanup_done;
            uint32_t pending, pending_frame, pending_sync, pending_fence;
        } session;
        VkDevice handle;
        PFN_vkDestroyDevice destroy;
        PFN_vkGetDeviceQueue get_queue;
        PFN_vkGetDeviceProcAddr gdpa;
        struct native_submit submit;
        struct native_interop interop;
        struct native_renderer renderer;
        VkPhysicalDevice physical;
        PFN_vkGetPhysicalDeviceImageFormatProperties2 image_properties;
        PFN_vkGetPhysicalDeviceExternalFenceProperties fence_properties;
        uint32_t id, family, count, queue_flags;
        VkQueue queues[MB_DEVICE_MAX_QUEUES];
        uint32_t queue_ids[MB_DEVICE_MAX_QUEUES];
    } logical[MB_MAX_LOGICAL_DEVICES];
};
/* Only failed diagnostics enter this list. Native parents/callback storage must
 * survive the guest's exit while Android still owns the producer allocation. */
static struct vk_session *quarantined_sessions;

static const char *const query_extensions[] = {
    "VK_KHR_get_physical_device_properties2", "VK_KHR_external_memory_capabilities",
    "VK_KHR_external_semaphore_capabilities", "VK_KHR_external_fence_capabilities",
};
static VkResult native_global(void *, uint32_t *, uint32_t *, VkExtensionProperties *);

static void native_close_device(struct native_device *d);
static VkResult native_session_end(struct native_device *d);
static void native_session_log(struct native_device *d, const char *stage);
static int native_verbose(struct native_device *d) { return !d->session.enabled || d->session.presented < 3; }

static int close_session(struct vk_session *s) {
    int retained = 0;
    for (unsigned i = 0; i < MB_MAX_LOGICAL_DEVICES; ++i)
        if (s->logical[i].handle) {
            native_close_device(&s->logical[i]);
            retained |= !!s->logical[i].handle;
        }
    if (retained) return 0;
    if (s->instance) { s->destroy(s->instance, NULL); LOG("destroyed Vulkan instance"); }
    if (s->library) { dlclose(s->library); LOG("closed Android libvulkan"); }
    memset(s, 0, sizeof(*s));
    return 1;
}

static uint32_t open_session(struct vk_session *s, uint32_t api, VkResult *result, int capabilities) {
    /* The glibc instance API remains 1.0; v3 enables only native query extensions. */
    if (VK_API_VERSION_VARIANT(api) || VK_API_VERSION_MAJOR(api) != 1 ||
        VK_API_VERSION_MINOR(api) != 0) {
        *result = VK_ERROR_INCOMPATIBLE_DRIVER;
        return MB_VULKAN_ERROR;
    }
    s->library = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!s->library) { ERROR("Android libvulkan load failed: %s", dlerror()); return MB_LOADER_ERROR; }
    LOG("loaded Android /system/lib64/libvulkan.so");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(s->library, "vkGetInstanceProcAddr");
    s->gipa = gipa;
    PFN_vkCreateInstance create = gipa ? (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance") : NULL;
    s->destroy = (PFN_vkDestroyInstance)dlsym(s->library, "vkDestroyInstance");
    if (!create || !s->destroy) { ERROR("system loader missing instance entry points"); return MB_LOADER_ERROR; }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "DroidDeck query broker", .apiVersion = api };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    const char *enabled[4];
    if (capabilities) {
        uint32_t version, n;
        VkExtensionProperties extensions[MB_MAX_EXTENSIONS];
        *result = native_global(s->library, &version, &n, extensions);
        if (*result != VK_SUCCESS) return MB_VULKAN_ERROR;
        /* Only the broker uses native 1.1 queries; the glibc ICD still supports API 1.0.
         * On a 1.0 loader, enable the actually-reported KHR query extensions instead. */
        app.apiVersion = version >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
        for (unsigned i = 0; i < 4; ++i) {
            if (mb_has_extension(extensions, n, query_extensions[i])) {
                enabled[info.enabledExtensionCount++] = query_extensions[i];
                s->enabled_queries |= 1u << i;
            }
        }
        info.ppEnabledExtensionNames = enabled;
        LOG("capability instance: native API=%u.%u.%u query extensions=%u", VK_VERSION_MAJOR(app.apiVersion),
            VK_VERSION_MINOR(app.apiVersion), VK_VERSION_PATCH(app.apiVersion), info.enabledExtensionCount);
    }
    s->native_api = app.apiVersion;
    VkInstance instance = VK_NULL_HANDLE;
    *result = create(&info, NULL, &instance);
    LOG("vkCreateInstance result=%d", (int)*result);
    if (*result != VK_SUCCESS) return MB_VULKAN_ERROR;
    s->instance = instance;
    s->enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    s->properties = (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
    if (!s->enumerate || !s->properties) {
        ERROR("system loader missing device query entry points");
        return MB_LOADER_ERROR;
    }
    return MB_OK;
}

static uint32_t list_devices(struct vk_session *s, VkResult *result) {
    if (s->listed) return MB_OK;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        s->count = 0;
        *result = s->enumerate(s->instance, &s->count, NULL);
        LOG("physical-device count=%u result=%d", s->count, (int)*result);
        if (*result != VK_SUCCESS) return MB_VULKAN_ERROR;
        if (s->count > MB_MAX_DEVICES) return MB_INTERNAL_ERROR;
        if (!s->count) { s->listed = 1; return MB_OK; }
        *result = s->enumerate(s->instance, &s->count, s->devices);
        LOG("physical-device list count=%u result=%d", s->count, (int)*result);
        if (*result == VK_INCOMPLETE) continue;
        if (*result != VK_SUCCESS) return MB_VULKAN_ERROR;
        if (s->count > MB_MAX_DEVICES) return MB_INTERNAL_ERROR;
        s->listed = 1;
        return MB_OK;
    }
    return MB_VULKAN_ERROR;
}

static void get_properties(struct vk_session *s, uint32_t index, VkPhysicalDeviceProperties *p) {
    memset(p, 0, sizeof(*p));
    s->properties(s->devices[index], p);
    p->deviceName[MB_NAME_BYTES - 1] = 0;
    LOG("properties[%u]: name=%s vendor=0x%x device=0x%x api=%u driver=%u type=%u",
        index, p->deviceName, p->vendorID, p->deviceID, p->apiVersion, p->driverVersion, (unsigned)p->deviceType);
}

#include "capability_queries.h"
#include "interop_commands.h"
#include "submit_commands.h"
#include "renderer_commands.h"
#include "session_commands.h"
#include "device_commands.h"

/* Preserve the original version-1 one-shot probe and its six-field reply. */
static uint32_t query_devices(uint8_t *payload) {
    struct vk_session *s = calloc(1, sizeof(*s));
    if (!s) {
        mb_put_u32(payload, MB_INTERNAL_ERROR); mb_put_u32(payload + 4, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY); mb_put_u32(payload + 8, 0);
        return MB_PREFIX_BYTES;
    }
    VkResult result = VK_SUCCESS;
    uint32_t status = open_session(s, VK_API_VERSION_1_0, &result, 0), count = 0;
    if (status == MB_OK) status = list_devices(s, &result);
    if (status == MB_OK && !s->count) status = MB_NO_DEVICES;
    if (status == MB_OK) {
        count = s->count;
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties p;
            get_properties(s, i, &p);
            uint8_t *record = payload + MB_PREFIX_BYTES + i * MB_RECORD_BYTES;
            memcpy(record, p.deviceName, strnlen(p.deviceName, MB_NAME_BYTES - 1));
            uint8_t *v = record + MB_NAME_BYTES;
            mb_put_u32(v, p.vendorID); mb_put_u32(v + 4, p.deviceID);
            mb_put_u32(v + 8, p.apiVersion); mb_put_u32(v + 12, p.driverVersion);
            mb_put_u32(v + 16, (uint32_t)p.deviceType);
        }
    }
    close_session(s); free(s);
    if (status != MB_OK) ERROR("query failed: status=%u VkResult=%d", status, (int)result);
    mb_put_u32(payload, status); mb_put_u32(payload + 4, (uint32_t)result); mb_put_u32(payload + 8, count);
    return MB_PREFIX_BYTES + count * MB_RECORD_BYTES;
}

static void serve(int fd) {
    struct vk_session *s = calloc(1, sizeof(*s));
    uint8_t *request = malloc(MB_RENDERER_MAX_REQUEST);
    if (!s || !request) { free(s); free(request); ERROR("client allocation failed"); return; }
    for (;;) {
        uint8_t header[MB_HEADER_BYTES], payload[MB_CAP_MAX_PAYLOAD] = {0};
        if (mb_read(fd, header, sizeof(header))) break;
        uint32_t version = mb_get_u32(header + 4), op = mb_get_u32(header + 8);
        uint32_t request_bytes = mb_get_u32(header + 12), bytes = MB_PREFIX_BYTES;
        uint32_t status = MB_PROTOCOL_ERROR, count = 0;
        VkResult result = VK_SUCCESS;
        int finish = 0;
        if (mb_get_u32(header) != MB_MAGIC || request_bytes > MB_RENDERER_MAX_REQUEST) {
            finish = 1;
        } else if (version == MB_VERSION && op == MB_ENUMERATE && !request_bytes && !s->library) {
            bytes = query_devices(payload);
            mb_header(header, bytes);
            if (mb_write(fd, header, sizeof(header)) || mb_write(fd, payload, bytes))
                ERROR("write probe response: %s", strerror(errno));
            break;
        } else if ((version != MB_SESSION_VERSION && version != MB_CAP_VERSION && version != MB_DEVICE_VERSION && version != MB_SUBMIT_VERSION && version != MB_INTEROP_VERSION && version != MB_RENDERER_VERSION) ||
                   (s->wire_version && version != s->wire_version) ||
                   (request_bytes && mb_read(fd, request, request_bytes))) {
            finish = 1;
        } else if (version >= MB_CAP_VERSION && op == MB_GLOBAL && !request_bytes && !s->library) {
            void *library = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
            uint32_t api = 0;
            VkExtensionProperties extensions[MB_MAX_EXTENSIONS];
            status = MB_LOADER_ERROR;
            if (library) {
                result = native_global(library, &api, &count, extensions);
                status = result == VK_SUCCESS ? MB_OK : MB_VULKAN_ERROR;
                if (status == MB_OK) {
                    mb_put_u32(payload + MB_PREFIX_BYTES, api);
                    mb_encode_extensions(payload + MB_PREFIX_BYTES + 4, extensions, count);
                    bytes += 4 + count * MB_EXTENSION_BYTES;
                    LOG("Android loader API=%u.%u.%u instance extensions=%u", VK_VERSION_MAJOR(api),
                        VK_VERSION_MINOR(api), VK_VERSION_PATCH(api), count);
                } else count = 0;
                dlclose(library);
            } else ERROR("Android global inventory load failed: %s", dlerror());
            finish = 1;
        } else if (op == MB_CREATE && request_bytes == 4 && !s->library) {
            status = open_session(s, mb_get_u32(request), &result, version >= MB_CAP_VERSION);
            s->wire_version = version;
            finish = status != MB_OK;
            /* Live instances may idle. Stop shuts down every connection to wake reads-> */
            struct timeval no_timeout = {0};
            if (!finish && setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout))) {
                status = MB_INTERNAL_ERROR; finish = 1;
            }
        } else if (op == MB_LIST && !request_bytes && s->instance) {
            status = list_devices(s, &result);
            if (status == MB_OK) {
                count = s->count; bytes += count * 4;
                for (uint32_t i = 0; i < count; ++i) mb_put_u32(payload + MB_PREFIX_BYTES + i * 4, i + 1);
            }
        } else if (op == MB_PROPERTIES && request_bytes == 4 && s->instance && s->listed) {
            uint32_t id = mb_get_u32(request);
            if (id && id <= s->count) {
                VkPhysicalDeviceProperties p;
                get_properties(s, id - 1, &p);
                mb_encode_properties(payload + MB_PREFIX_BYTES, &p);
                status = MB_OK; count = 1; bytes += MB_PROPERTIES_BYTES;
            } else finish = 1;
        } else if (version >= MB_CAP_VERSION && op == MB_CAPS && request_bytes == 4 && s->instance && s->listed) {
            uint32_t id = mb_get_u32(request);
            if (id && id <= s->count) {
                status = native_snapshot(s, id, payload + MB_PREFIX_BYTES, &result);
                if (status == MB_OK) { count = 1; bytes += MB_PROPERTIES_BYTES + MB_CAPS_BYTES; }
            } else finish = 1;
        } else if (version >= MB_CAP_VERSION && op >= MB_FORMAT && op <= MB_SPARSE && s->instance && s->listed) {
            uint32_t extra = 0;
            status = native_extra(s, op, request, request_bytes, payload + MB_PREFIX_BYTES, &extra, &count, &result);
            if (status == MB_OK) bytes += extra;
            else { count = 0; if (status == MB_UNSUPPORTED) result = VK_ERROR_FEATURE_NOT_PRESENT; }
        } else if (version >= MB_DEVICE_VERSION && op >= MB_DEVICE_CREATE && op <= MB_DEVICE_DESTROY && s->instance && s->listed) {
            uint32_t extra = 0;
            status = native_device_command(s, op, request, request_bytes, payload + MB_PREFIX_BYTES, &extra, &count, &result);
            if (status == MB_OK) bytes += extra;
            if (op == MB_DEVICE_DESTROY && result == VK_TIMEOUT) finish = 1;
        } else if (version >= MB_SUBMIT_VERSION && op >= MB_POOL_CREATE && op <= MB_QUEUE_SUBMIT && s->instance && s->listed) {
            uint32_t extra = 0;
            status = native_submit_command(s, op, request, request_bytes, payload + MB_PREFIX_BYTES, &extra, &count, &result);
            if (status == MB_OK) bytes += extra;
        } else if (version == MB_RENDERER_VERSION && op >= MB_SESSION_BEGIN && op <= MB_SESSION_STATS && s->instance && s->listed) {
            uint32_t extra = 0;
            status = native_session_command(s, op, request, request_bytes, payload + MB_PREFIX_BYTES, &extra, &count, &result);
            if (status == MB_OK) bytes += extra;
            /* A failed release wait ends production on this connection. */
            if (op == MB_SESSION_END && result == VK_TIMEOUT) finish = 1;
        } else if (version >= MB_INTEROP_VERSION && op >= MB_BUFFER_CREATE && op <= MB_AHB_PRESENT && s->instance && s->listed) {
            uint32_t extra = 0;
            status = native_interop_command(s, op, request, request_bytes, payload + MB_PREFIX_BYTES, &extra, &count, &result);
            if (status == MB_OK) bytes += extra;
        } else if (version == MB_RENDERER_VERSION && op >= MB_RENDERER_SEMAPHORE_CREATE && op <= MB_RENDERER_IDLE && s->instance && s->listed) {
            uint32_t extra = 0;
            status = native_renderer_command(s, op, request, request_bytes, payload + MB_PREFIX_BYTES, &extra, &count, &result);
            if (status == MB_OK) bytes += extra;
        } else if (op == MB_DESTROY && !request_bytes && s->instance) {
            if (!close_session(s)) result = VK_TIMEOUT;
            status = MB_OK; finish = 1;
        } else {
            finish = 1;
        }
        if (version == MB_RENDERER_VERSION && status == MB_PROTOCOL_ERROR) finish = 1;
        mb_put_u32(payload, status); mb_put_u32(payload + 4, (uint32_t)result); mb_put_u32(payload + 8, count);
        if (version == MB_VERSION) mb_header(header, bytes);
        else mb_session_header(header, op, bytes);
        mb_put_u32(header + 4, version);
        if (status != MB_OK || result != VK_SUCCESS) session_quiet = 0;
        int verbose = 1;
        for (unsigned i = 0; i < MB_MAX_LOGICAL_DEVICES; ++i) if (s->logical[i].session.enabled && !native_verbose(&s->logical[i])) verbose = 0;
        if (verbose || status != MB_OK || result != VK_SUCCESS) LOG("query RPC opcode=%u status=%u VkResult=%d count=%u", op, status, (int)result, count);
        if (mb_write(fd, header, sizeof(header)) || mb_write(fd, payload, bytes)) {
            ERROR("write response: %s", strerror(errno)); break;
        }
        if (finish) break;
    }
    if (close_session(s)) free(s);
    else {
        pthread_mutex_lock(&lock); s->quarantined_next = quarantined_sessions;
        quarantined_sessions = s; pthread_mutex_unlock(&lock);
        ERROR("failed session quarantined: Android-owned allocations and native parents retained; no PASS");
    }
    free(request);
}

static void *client_main(void *arg) {
    struct client_slot *slot = arg;
    int fd = slot->fd;
    struct ucred credentials;
    socklen_t length = sizeof(credentials);
    struct timeval timeout = { .tv_sec = 5 };
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) || credentials.uid != getuid()) {
        ERROR("rejected client: credentials must match app uid");
    } else if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
               setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) {
        ERROR("client socket timeout: %s", strerror(errno));
    } else {
        LOG("accepted app-uid client pid=%d", credentials.pid);
        serve(fd);
    }
    pthread_mutex_lock(&lock);
    close(fd); slot->fd = -1; slot->done = 1;
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void *broker_main(void *unused) {
    (void)unused;
    for (;;) {
        int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0 && errno == EINTR) continue;
        pthread_mutex_lock(&lock);
        if (stopping || fd < 0) {
            if (fd >= 0) close(fd);
            if (!stopping) ERROR("accept: %s", strerror(errno));
            pthread_mutex_unlock(&lock);
            break;
        }
        struct client_slot *slot = NULL;
        for (unsigned i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i].started && clients[i].done) {
                pthread_join(clients[i].worker, NULL);
                memset(&clients[i], 0, sizeof(clients[i]));
            }
            if (!clients[i].started && !slot) slot = &clients[i];
        }
        if (!slot) {
            ERROR("connection limit reached"); close(fd);
        } else {
            slot->fd = fd; slot->started = 1;
            int rc = pthread_create(&slot->worker, NULL, client_main, slot);
            if (rc) { ERROR("client thread: %s", strerror(rc)); close(fd); memset(slot, 0, sizeof(*slot)); }
        }
        pthread_mutex_unlock(&lock);
    }
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i].started && clients[i].fd >= 0) shutdown(clients[i].fd, SHUT_RDWR);
    pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i].started) pthread_join(clients[i].worker, NULL);
    pthread_mutex_lock(&lock);
    memset(clients, 0, sizeof(clients));
    close(listener); listener = -1;
    if (unlink(socket_path) && errno != ENOENT) ERROR("unlink socket: %s", strerror(errno));
    pthread_mutex_unlock(&lock);
    LOG("broker thread cleanup complete");
    return NULL;
}

static void throw_io(JNIEnv *env, const char *message) {
    jclass type = (*env)->FindClass(env, "java/io/IOException");
    if (type) (*env)->ThrowNew(env, type, message);
}

JNIEXPORT void JNICALL
Java_com_droiddeck_launcher_gpu_SystemVulkanBroker_nativeStart(JNIEnv *env, jclass clazz, jstring path) {
    (void)clazz;
    const char *text = (*env)->GetStringUTFChars(env, path, NULL);
    if (!text) return;
    pthread_mutex_lock(&lock);
    if (running) {
        pthread_mutex_unlock(&lock);
        (*env)->ReleaseStringUTFChars(env, path, text);
        throw_io(env, "broker is already started");
        return;
    }
    int fd = -1, bound = 0;
    char error[256];
    if (text[0] != '/' || strlen(text) >= sizeof(socket_path)) {
        snprintf(error, sizeof(error), "broker requires an absolute Unix socket path shorter than %zu bytes", sizeof(socket_path));
        goto fail;
    }
    strcpy(socket_path, text);
    struct stat st;
    if (lstat(text, &st) == 0) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid != getuid()) {
            snprintf(error, sizeof(error), "refusing to replace non-socket or foreign socket"); goto fail;
        }
        if (unlink(text)) goto syscall_fail;
    } else if (errno != ENOENT) goto syscall_fail;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) goto syscall_fail;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    strcpy(address.sun_path, text);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address))) goto syscall_fail;
    bound = 1;
    if (chmod(text, 0600) || listen(fd, 4)) goto syscall_fail;
    listener = fd;
    stopping = 0;
    int rc = pthread_create(&thread, NULL, broker_main, NULL);
    if (rc) { errno = rc; listener = -1; goto syscall_fail; }
    running = 1;
    LOG("socket created and listening: %s (uid=%u mode=0600)", text, (unsigned)getuid());
    pthread_mutex_unlock(&lock);
    (*env)->ReleaseStringUTFChars(env, path, text);
    return;
syscall_fail:
    snprintf(error, sizeof(error), "broker socket/thread setup: %s", strerror(errno));
fail:
    if (fd >= 0) close(fd);
    if (bound) unlink(text);
    ERROR("%s", error);
    pthread_mutex_unlock(&lock);
    (*env)->ReleaseStringUTFChars(env, path, text);
    throw_io(env, error);
}

JNIEXPORT void JNICALL
Java_com_droiddeck_launcher_gpu_SystemVulkanBroker_nativeStop(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    pthread_mutex_lock(&lock);
    if (!running) { pthread_mutex_unlock(&lock); return; }
    stopping = 1;
    for (unsigned i = 0; i < MAX_CLIENTS; ++i)
        if (clients[i].started && clients[i].fd >= 0) shutdown(clients[i].fd, SHUT_RDWR);
    if (listener >= 0) shutdown(listener, SHUT_RDWR);
    pthread_mutex_unlock(&lock);
    /* Do not cancel a vendor Vulkan call; allow query cleanup before joining. */
    pthread_join(thread, NULL);
    pthread_mutex_lock(&lock);
    running = 0;
    pthread_mutex_unlock(&lock);
    LOG("broker stopped");
}
