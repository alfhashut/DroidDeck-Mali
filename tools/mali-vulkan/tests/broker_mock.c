/* Host-only harness: real broker code, mock Android loader/Vulkan/JNI. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <assert.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <vulkan/vulkan.h>
static void *mock_dlopen(const char *, int);
static void *mock_dlsym(void *, const char *);
static int mock_dlclose(void *);
static const char *mock_dlerror(void);
#define dlopen mock_dlopen
#define dlsym mock_dlsym
#define dlclose mock_dlclose
#define dlerror mock_dlerror
#include "../../../app/src/main/cpp/malivulkan/system_broker.c"
#undef dlopen
#undef dlsym
#undef dlclose
#undef dlerror
static atomic_int mode, creates, destroys, closes;
static int thrown;
int __android_log_print(int priority, const char *tag, const char *format, ...) {
    (void)priority;
    fprintf(stderr, "%s: ", tag);
    va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args);
    fputc('\n', stderr); return 0;
}
static VkResult mock_create(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator, VkInstance *out) {
    assert(info->enabledExtensionCount == 0 && allocator == NULL);
    ++creates;
    if (mode == 2) return VK_ERROR_INITIALIZATION_FAILED;
    *out = (VkInstance)malloc(1);
    assert(*out);
    return VK_SUCCESS;
}
static void mock_destroy(VkInstance instance, const VkAllocationCallbacks *allocator) {
    assert(instance && allocator == NULL); free(instance); ++destroys;
}
static VkResult mock_enumerate(VkInstance instance, uint32_t *count, VkPhysicalDevice *devices) {
    assert(instance);
    if (!devices) { *count = mode == 3 ? 0 : 1; return VK_SUCCESS; }
    assert(*count >= 1);
    *count = 1; devices[0] = (VkPhysicalDevice)(uintptr_t)0x42;
    return mode == 4 ? VK_INCOMPLETE : VK_SUCCESS;
}
static void mock_properties(VkPhysicalDevice device, VkPhysicalDeviceProperties *p) {
    assert(device == (VkPhysicalDevice)(uintptr_t)0x42);
    memset(p, 0, sizeof(*p)); strcpy(p->deviceName, "HOST TEST ONLY");
    p->vendorID = 0x13b5; p->deviceID = 0x74021000; p->apiVersion = VK_MAKE_API_VERSION(0, 1, 1, 131);
    p->driverVersion = 109051904; p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    p->limits.maxImageDimension2D = 8192;
    p->limits.bufferImageGranularity = UINT64_C(0x123456789);
    p->limits.minMemoryMapAlignment = 4096;
    p->limits.minTexelOffset = -8;
    p->limits.timestampPeriod = 2.5f;
    p->sparseProperties.residencyStandard2DBlockShape = VK_TRUE;
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i) p->pipelineCacheUUID[i] = (uint8_t)i;
}
static PFN_vkVoidFunction mock_gipa(VkInstance instance, const char *name) {
    (void)instance;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)mock_create;
    if (!strcmp(name, "vkEnumeratePhysicalDevices")) return (PFN_vkVoidFunction)mock_enumerate;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties") && mode != 5) return (PFN_vkVoidFunction)mock_properties;
    return NULL;
}
static void *mock_dlopen(const char *path, int flags) {
    (void)flags; assert(!strcmp(path, "/system/lib64/libvulkan.so"));
    return mode == 1 ? NULL : (void *)(uintptr_t)1;
}
static void *mock_dlsym(void *library, const char *name) {
    assert(library);
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (void *)mock_gipa;
    if (!strcmp(name, "vkDestroyInstance")) return (void *)mock_destroy;
    return NULL;
}
static int mock_dlclose(void *library) { assert(library); ++closes; return 0; }
static const char *mock_dlerror(void) { return "test loader failure"; }
static const char *jni_text(JNIEnv *env, jstring text, jboolean *copy) {
    (void)env; (void)copy; return (const char *)text;
}
static void jni_release(JNIEnv *env, jstring text, const char *utf) { (void)env; (void)text; (void)utf; }
static jclass jni_class(JNIEnv *env, const char *name) { (void)env; (void)name; return (jclass)(uintptr_t)1; }
static jint jni_throw(JNIEnv *env, jclass type, const char *message) {
    (void)env; (void)type; fprintf(stderr, "expected JNI error: %s\n", message); ++thrown; return 0;
}
static const struct JNINativeInterface_ jni = {
    .GetStringUTFChars = jni_text, .ReleaseStringUTFChars = jni_release,
    .FindClass = jni_class, .ThrowNew = jni_throw
};
static JNIEnv environment = &jni;
static const char *path;
static void start(void) { Java_com_droiddeck_launcher_gpu_SystemVulkanBroker_nativeStart(&environment, NULL, (jstring)path); }
static void stop(void) { Java_com_droiddeck_launcher_gpu_SystemVulkanBroker_nativeStop(&environment, NULL); }
static int connect_client(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); assert(fd >= 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX }; strcpy(addr.sun_path, path);
    assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0); return fd;
}
static void request(uint32_t expected_status, int bad_version) {
    int fd = connect_client(); uint8_t header[MB_HEADER_BYTES], payload[MB_MAX_PAYLOAD];
    mb_header(header, 0); if (bad_version) mb_put_u32(header + 4, 2);
    assert(mb_write(fd, header, sizeof(header)) == 0);
    assert(mb_read(fd, header, sizeof(header)) == 0);
    uint32_t bytes = mb_get_u32(header + 12); assert(bytes <= sizeof(payload));
    assert(mb_read(fd, payload, bytes) == 0); assert(mb_get_u32(payload) == expected_status);
    if (expected_status == MB_OK) {
        assert(mb_get_u32(payload + 8) == 1);
        assert(!strcmp((char *)payload + MB_PREFIX_BYTES, "HOST TEST ONLY"));
        assert(mb_get_u32(payload + MB_PREFIX_BYTES + MB_NAME_BYTES) == 0x13b5);
    } else assert(mb_get_u32(payload + 8) == 0);
    close(fd);
}
int main(int argc, char **argv) {
    assert(argc >= 3);
    path = argv[2];
    if (!strcmp(argv[1], "--serve")) {
        if (argc > 3) mode = atoi(argv[3]);
        start(); assert(!thrown);
        puts("READY"); fflush(stdout);
        (void)getchar();
        stop();
        printf("CLEANUP creates=%d destroys=%d closes=%d\n", creates, destroys, closes);
        return 0;
    }
    assert(argc == 4);
    start(); assert(thrown == 0);
    struct stat st; assert(lstat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    request(MB_OK, 0); request(MB_PROTOCOL_ERROR, 1);
    pid_t child = fork(); assert(child >= 0);
    if (child == 0) { execl(argv[3], "probe", path, (char *)NULL); _exit(127); }
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    mode = 1; request(MB_LOADER_ERROR, 0);
    mode = 2; request(MB_VULKAN_ERROR, 0);
    mode = 3; request(MB_NO_DEVICES, 0);
    mode = 4; request(MB_VULKAN_ERROR, 0);
    mode = 5; request(MB_LOADER_ERROR, 0);
    stop(); assert(lstat(path, &st) != 0 && errno == ENOENT);
    assert(creates == 6 && destroys == 5 && closes == 6);
    mode = 0;
    start(); stop(); /* Stop while accept is blocked. */
    start();
    int fd = connect_client();
    for (int i = 0; i < 1000; ++i) {
        pthread_mutex_lock(&lock); int active = 0;
        for (unsigned j = 0; j < MAX_CLIENTS; ++j) active |= clients[j].started && clients[j].fd >= 0;
        pthread_mutex_unlock(&lock);
        if (active) break;
        usleep(1000);
    }
    stop(); close(fd); /* Stop while client request read is blocked. */
    FILE *file = fopen(path, "w"); assert(file); fclose(file);
    start(); assert(thrown == 1 && lstat(path, &st) == 0 && S_ISREG(st.st_mode));
    unlink(path);
    puts("native broker: success/error queries, instance cleanup, repeated start/stop, blocked read/accept, socket ownership checks passed (mock Vulkan backend)");
    return 0;
}
