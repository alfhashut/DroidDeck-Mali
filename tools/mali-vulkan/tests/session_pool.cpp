#include "mali_output_pool.hpp"
#include <cassert>
int main() {
    MaliOutputPool pool;
    assert(!pool.ready(0) && !pool.released(0) && !pool.presented(0));
    for (unsigned i = 0; i < 3; ++i) {
        assert(pool.acquire(i) == int(i));
        assert(!pool.presented(i));
        assert(pool.ready(i) && !pool.ready(i));
        assert(pool.presented(i) && !pool.presented(i));
    }
    assert(pool.acquire(0) == -1); // no recording into Android-owned output
    // A missing release leaves every owned slot unreusable. Time passing or
    // diagnostic failure is not a call to released().
    for (unsigned i = 0; i < 3; ++i) assert(pool.androidOwned(i) && !pool.free(i));
    assert(pool.acquire(2) == -1);
    assert(!pool.ready(1) && !pool.released(3));
    for (unsigned frame = 0; frame < 600; ++frame) {
        unsigned i = frame % 3;
        assert(pool.released(i) && !pool.released(i));
        assert(pool.acquire(i) == int(i));
        assert(pool.ready(i) && pool.presented(i));
    }
    for (unsigned i = 0; i < 3; ++i) assert(pool.released(i) && pool.free(i));
}
