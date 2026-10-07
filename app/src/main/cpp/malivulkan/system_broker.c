/* Opt-in Android system Vulkan query broker. No rendering or driver override. */
#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "protocol.h"
#include "properties.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "MaliVulkanBroker", __VA_ARGS__)
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
    void *library;
    VkInstance instance;
    PFN_vkDestroyInstance destroy;
    PFN_vkEnumeratePhysicalDevices enumerate;
    PFN_vkGetPhysicalDeviceProperties properties;
    VkPhysicalDevice devices[MB_MAX_DEVICES];
    uint32_t count;
    int listed;
};

static void close_session(struct vk_session *s) {
    if (s->instance) { s->destroy(s->instance, NULL); LOG("destroyed Vulkan instance"); }
    if (s->library) { dlclose(s->library); LOG("closed Android libvulkan"); }
    memset(s, 0, sizeof(*s));
}

static uint32_t open_session(struct vk_session *s, uint32_t api, VkResult *result) {
    /* The proxy supports core instance queries at 1.0, with no extensions. */
    if (VK_API_VERSION_VARIANT(api) || VK_API_VERSION_MAJOR(api) != 1 ||
        VK_API_VERSION_MINOR(api) != 0) {
        *result = VK_ERROR_INCOMPATIBLE_DRIVER;
        return MB_VULKAN_ERROR;
    }
    s->library = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!s->library) { ERROR("Android libvulkan load failed: %s", dlerror()); return MB_LOADER_ERROR; }
    LOG("loaded Android /system/lib64/libvulkan.so");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(s->library, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create = gipa ? (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance") : NULL;
    s->destroy = (PFN_vkDestroyInstance)dlsym(s->library, "vkDestroyInstance");
    if (!create || !s->destroy) { ERROR("system loader missing instance entry points"); return MB_LOADER_ERROR; }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "DroidDeck query broker", .apiVersion = api };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
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

/* Preserve the original version-1 one-shot probe and its six-field reply. */
static uint32_t query_devices(uint8_t *payload) {
    struct vk_session s = {0};
    VkResult result = VK_SUCCESS;
    uint32_t status = open_session(&s, VK_API_VERSION_1_0, &result), count = 0;
    if (status == MB_OK) status = list_devices(&s, &result);
    if (status == MB_OK && !s.count) status = MB_NO_DEVICES;
    if (status == MB_OK) {
        count = s.count;
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties p;
            get_properties(&s, i, &p);
            uint8_t *record = payload + MB_PREFIX_BYTES + i * MB_RECORD_BYTES;
            memcpy(record, p.deviceName, strnlen(p.deviceName, MB_NAME_BYTES - 1));
            uint8_t *v = record + MB_NAME_BYTES;
            mb_put_u32(v, p.vendorID); mb_put_u32(v + 4, p.deviceID);
            mb_put_u32(v + 8, p.apiVersion); mb_put_u32(v + 12, p.driverVersion);
            mb_put_u32(v + 16, (uint32_t)p.deviceType);
        }
    }
    close_session(&s);
    if (status != MB_OK) ERROR("query failed: status=%u VkResult=%d", status, (int)result);
    mb_put_u32(payload, status); mb_put_u32(payload + 4, (uint32_t)result); mb_put_u32(payload + 8, count);
    return MB_PREFIX_BYTES + count * MB_RECORD_BYTES;
}

static void serve(int fd) {
    struct vk_session s = {0};
    for (;;) {
        uint8_t header[MB_HEADER_BYTES], request[4], payload[MB_MAX_PAYLOAD] = {0};
        if (mb_read(fd, header, sizeof(header))) break;
        uint32_t version = mb_get_u32(header + 4), op = mb_get_u32(header + 8);
        uint32_t request_bytes = mb_get_u32(header + 12), bytes = MB_PREFIX_BYTES;
        uint32_t status = MB_PROTOCOL_ERROR, count = 0;
        VkResult result = VK_SUCCESS;
        int finish = 0;
        if (mb_get_u32(header) != MB_MAGIC || request_bytes > sizeof(request)) {
            finish = 1;
        } else if (version == MB_VERSION && op == MB_ENUMERATE && !request_bytes && !s.library) {
            bytes = query_devices(payload);
            mb_header(header, bytes);
            if (mb_write(fd, header, sizeof(header)) || mb_write(fd, payload, bytes))
                ERROR("write probe response: %s", strerror(errno));
            break;
        } else if (version != MB_SESSION_VERSION ||
                   (request_bytes && mb_read(fd, request, request_bytes))) {
            finish = 1;
        } else if (op == MB_CREATE && request_bytes == 4 && !s.library) {
            status = open_session(&s, mb_get_u32(request), &result);
            finish = status != MB_OK;
            /* Live instances may idle. Stop shuts down every connection to wake reads. */
            struct timeval no_timeout = {0};
            if (!finish && setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout))) {
                status = MB_INTERNAL_ERROR; finish = 1;
            }
        } else if (op == MB_LIST && !request_bytes && s.instance) {
            status = list_devices(&s, &result);
            if (status == MB_OK) {
                count = s.count; bytes += count * 4;
                for (uint32_t i = 0; i < count; ++i) mb_put_u32(payload + MB_PREFIX_BYTES + i * 4, i + 1);
            }
        } else if (op == MB_PROPERTIES && request_bytes == 4 && s.instance && s.listed) {
            uint32_t id = mb_get_u32(request);
            if (id && id <= s.count) {
                VkPhysicalDeviceProperties p;
                get_properties(&s, id - 1, &p);
                mb_encode_properties(payload + MB_PREFIX_BYTES, &p);
                status = MB_OK; count = 1; bytes += MB_PROPERTIES_BYTES;
            } else finish = 1;
        } else if (op == MB_DESTROY && !request_bytes && s.instance) {
            close_session(&s); status = MB_OK; finish = 1;
        } else {
            finish = 1;
        }
        mb_put_u32(payload, status); mb_put_u32(payload + 4, (uint32_t)result); mb_put_u32(payload + 8, count);
        if (version == MB_VERSION) mb_header(header, bytes);
        else mb_session_header(header, op, bytes);
        LOG("query RPC opcode=%u status=%u VkResult=%d count=%u", op, status, (int)result, count);
        if (mb_write(fd, header, sizeof(header)) || mb_write(fd, payload, bytes)) {
            ERROR("write response: %s", strerror(errno)); break;
        }
        if (finish) break;
    }
    close_session(&s);
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
