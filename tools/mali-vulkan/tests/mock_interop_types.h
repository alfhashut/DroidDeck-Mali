/* Explicit host model. GPU mutations occur only in mock_execute(), not recording. */
static atomic_int buffers_created, buffers_destroyed, images_created, images_destroyed;
static atomic_int memories_created, memories_freed, ahb_created, ahb_freed, cpu_locks, consumers, flushes, invalidates;
struct AHardwareBuffer { AHardwareBuffer_Desc desc; unsigned refs; uint8_t *pixels; };
struct mock_memory { struct mock_logical *device; uint64_t size; uint32_t type; int mapped; uint8_t *gpu, *host; AHardwareBuffer *ahb; };
struct mock_storage { struct mock_logical *device; uint64_t size, offset; uint32_t width, height, type; int image, external; struct mock_memory *memory; };
struct mock_render;
struct mock_op { struct mock_render *renderer_set; unsigned kind; struct mock_storage *buffer, *image; uint64_t offset, size; uint32_t pattern; uint8_t color[4]; };
static void mock_renderer_execute(struct mock_op *);
static void mock_timeline_signal(struct mock_object *);
static void mock_interop_execute(struct mock_object *c);
