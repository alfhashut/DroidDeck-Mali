#ifndef MB_MOCK_WINDOW_H
#define MB_MOCK_WINDOW_H
#include <stdint.h>
typedef struct ANativeWindow ANativeWindow;
#define ANATIVEWINDOW_TRANSFORM_IDENTITY 0
void ANativeWindow_acquire(ANativeWindow *);
void ANativeWindow_release(ANativeWindow *);
int32_t ANativeWindow_getWidth(ANativeWindow *);
int32_t ANativeWindow_getHeight(ANativeWindow *);
#endif
