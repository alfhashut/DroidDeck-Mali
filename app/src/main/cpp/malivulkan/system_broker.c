/* Opt-in query broker inside the Android process. No rendering or driver override. */
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

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "MaliVulkanBroker", __VA_ARGS__)
#define ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "MaliVulkanBroker", __VA_ARGS__)

_Static_assert(VK_MAX_PHYSICAL_DEVICE_NAME_SIZE == MB_NAME_BYTES, "device name size");

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static int listener = -1, client = -1, running, stopping;
static char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

static uint32_t query_devices(uint8_t *payload) {
    uint32_t status = MB_LOADER_ERROR, count = 0;
    int instance_created = 0;
    VkResult result = VK_SUCCESS;
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkDestroyInstance destroy = NULL;
    void *library = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { ERROR("Android libvulkan load failed: %s", dlerror()); goto done; }
    LOG("loaded Android /system/lib64/libvulkan.so");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create = gipa ? (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance") : NULL;
    /* Resolve destruction before creation, so every successful instance can be cleaned up. */
    destroy = (PFN_vkDestroyInstance)dlsym(library, "vkDestroyInstance");
    if (!create || !destroy) { ERROR("system loader missing instance entry points"); goto done; }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "DroidDeck broker checkpoint 1", .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app };
    result = create(&info, NULL, &instance);
    LOG("vkCreateInstance result=%d", (int)result);
    status = MB_VULKAN_ERROR;
    if (result != VK_SUCCESS) goto done;
    instance_created = 1;
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties properties = (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
    if (!enumerate || !properties) {
        status = MB_LOADER_ERROR; ERROR("system loader missing device query entry points"); goto done;
    }
    VkPhysicalDevice devices[MB_MAX_DEVICES];
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        count = 0;
        result = enumerate(instance, &count, NULL);
        LOG("physical-device count=%u result=%d", count, (int)result);
        if (result != VK_SUCCESS) goto done;
        if (!count) { status = MB_NO_DEVICES; goto done; }
        if (count > MB_MAX_DEVICES) {
            status = MB_INTERNAL_ERROR; ERROR("device count exceeds protocol limit %u", MB_MAX_DEVICES); goto done;
        }
        result = enumerate(instance, &count, devices);
        LOG("physical-device list count=%u result=%d", count, (int)result);
        if (result == VK_INCOMPLETE) continue;
        if (result != VK_SUCCESS) goto done;
        if (!count) { status = MB_NO_DEVICES; goto done; }
        if (count > MB_MAX_DEVICES) { status = MB_INTERNAL_ERROR; goto done; }
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties p = {0};
            properties(devices[i], &p);
            uint8_t *record = payload + MB_PREFIX_BYTES + i * MB_RECORD_BYTES;
            memcpy(record, p.deviceName, strnlen(p.deviceName, MB_NAME_BYTES - 1));
            record[MB_NAME_BYTES - 1] = 0;
            uint8_t *v = record + MB_NAME_BYTES;
            mb_put_u32(v, p.vendorID); mb_put_u32(v + 4, p.deviceID);
            mb_put_u32(v + 8, p.apiVersion); mb_put_u32(v + 12, p.driverVersion);
            mb_put_u32(v + 16, (uint32_t)p.deviceType);
            LOG("properties[%u]: name=%s vendor=0x%x device=0x%x api=%u driver=%u type=%u",
                i, (char *)record, p.vendorID, p.deviceID, p.apiVersion, p.driverVersion, (unsigned)p.deviceType);
        }
        status = MB_OK;
        break;
    }
done:
    if (instance_created) {
        destroy(instance, NULL);
        LOG("destroyed Vulkan instance");
    }
    if (library) { dlclose(library); LOG("closed Android libvulkan"); }
    if (status != MB_OK) {
        ERROR("query failed: status=%u VkResult=%d", status, (int)result);
        count = 0;
    }
    mb_put_u32(payload, status); mb_put_u32(payload + 4, (uint32_t)result);
    mb_put_u32(payload + 8, count);
    return MB_PREFIX_BYTES + count * MB_RECORD_BYTES;
}

static void serve(int fd) {
    uint8_t header[MB_HEADER_BYTES], payload[MB_MAX_PAYLOAD] = {0};
    if (mb_read(fd, header, sizeof(header))) { ERROR("read request: %s", strerror(errno)); return; }
    uint32_t bytes = MB_PREFIX_BYTES;
    if (mb_get_u32(header) != MB_MAGIC || mb_get_u32(header + 4) != MB_VERSION ||
        mb_get_u32(header + 8) != MB_ENUMERATE || mb_get_u32(header + 12) != 0) {
        ERROR("invalid request magic/version/opcode/length");
        mb_put_u32(payload, MB_PROTOCOL_ERROR);
    } else {
        bytes = query_devices(payload);
    }
    mb_header(header, bytes);
    if (mb_write(fd, header, sizeof(header)) || mb_write(fd, payload, bytes))
        ERROR("write response: %s", strerror(errno));
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
        client = fd;
        pthread_mutex_unlock(&lock);
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
        close(fd);
        client = -1;
        pthread_mutex_unlock(&lock);
    }
    pthread_mutex_lock(&lock);
    close(listener);
    listener = -1;
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
    if (client >= 0) shutdown(client, SHUT_RDWR);
    if (listener >= 0) shutdown(listener, SHUT_RDWR);
    pthread_mutex_unlock(&lock);
    /* Do not cancel a vendor Vulkan call; allow query cleanup before joining. */
    pthread_join(thread, NULL);
    pthread_mutex_lock(&lock);
    running = 0;
    pthread_mutex_unlock(&lock);
    LOG("broker stopped");
}
