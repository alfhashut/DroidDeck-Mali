/* Host test shim; Android builds use the real NDK header. */
#ifndef MALI_TEST_ANDROID_LOG_H
#define MALI_TEST_ANDROID_LOG_H
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_ERROR 6
int __android_log_print(int priority, const char *tag, const char *format, ...);
#endif
