/// @brief Test that enabling multiscale without specifying a tile size doesn't
/// crash.

#include "device/hal/device.manager.h"
#include "acquire.h"
#include "platform.h" // clock
#include "logger.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

void
reporter(int is_error,
         const char* file,
         int line,
         const char* function,
         const char* msg)
{
    fprintf(is_error ? stderr : stdout,
            "%s%s(%d) - %s: %s\n",
            is_error ? "ERROR " : "",
            file,
            line,
            function,
            msg);
}

/// Helper for passing size static strings as function args.
/// For a function: `f(char*,size_t)` use `f(SIZED("hello"))`.
/// Expands to `f("hello",5)`.
#define SIZED(str) str, sizeof(str) - 1

#define L (aq_logger)
#define LOG(...) L(0, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define ERR(...) L(1, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define EXPECT(e, ...)                                                         \
    do {                                                                       \
        if (!(e)) {                                                            \
            char buf[1 << 8] = { 0 };                                          \
            ERR(__VA_ARGS__);                                                  \
            snprintf(buf, sizeof(buf) - 1, __VA_ARGS__);                       \
            throw std::runtime_error(buf);                                     \
        }                                                                      \
    } while (0)
#define CHECK(e) EXPECT(e, "Expression evaluated as false: %s", #e)
#define DEVOK(e) CHECK(Device_Ok == (e))
#define OK(e) CHECK(AcquireStatus_Ok == (e))

/// example: `ASSERT_EQ(int,"%d",42,meaning_of_life())`
#define ASSERT_EQ(T, fmt, a, b)                                                \
    do {                                                                       \
        T a_ = (T)(a);                                                         \
        T b_ = (T)(b);                                                         \
        EXPECT(a_ == b_, "Expected %s==%s but " fmt "!=" fmt, #a, #b, a_, b_); \
    } while (0)

/// Check that a>b
/// example: `ASSERT_GT(int,"%d",43,meaning_of_life())`
#define ASSERT_GT(T, fmt, a, b)                                                \
    do {                                                                       \
        T a_ = (T)(a);                                                         \
        T b_ = (T)(b);                                                         \
        EXPECT(                                                                \
          a_ > b_, "Expected (%s) > (%s) but " fmt "<=" fmt, #a, #b, a_, b_);  \
    } while (0)

const static uint32_t frame_width = 640;
const static uint32_t frame_height = 480;
const static uint32_t chunk_planes = 128;
const static uint32_t max_frame_count = 100;

void
setup(AcquireRuntime* runtime)
{
    const char* filename = TEST ".zarr";
    auto dm = acquire_device_manager(runtime);
    CHECK(runtime);
    CHECK(dm);

    AcquireProperties props = {};
    OK(acquire_get_configuration(runtime, &props));

    DEVOK(device_manager_select(dm,
                                DeviceKind_Camera,
                                SIZED("simulated.*empty.*"),
                                &props.video[0].camera.identifier));
    DEVOK(device_manager_select(dm,
                                DeviceKind_Storage,
                                SIZED("Zarr"),
                                &props.video[0].storage.identifier));

    const struct PixelScale sample_spacing_um = { 1, 1 };

    storage_properties_init(&props.video[0].storage.settings,
                            0,
                            (char*)filename,
                            strlen(filename) + 1,
                            nullptr,
                            0,
                            sample_spacing_um);

    storage_properties_set_chunking_props(&props.video[0].storage.settings,
                                          frame_width,
                                          frame_height,
                                          chunk_planes);

    storage_properties_set_enable_multiscale(&props.video[0].storage.settings,
                                             1);

    props.video[0].camera.settings.binning = 1;
    props.video[0].camera.settings.pixel_type = SampleType_u8;
    props.video[0].camera.settings.shape = { .x = frame_width,
                                             .y = frame_height };
    // we may drop frames with lower exposure
    props.video[0].camera.settings.exposure_time_us = 1e4;
    props.video[0].max_frame_count = max_frame_count;

    OK(acquire_configure(runtime, &props));
}

void
acquire(AcquireRuntime* runtime)
{
    const auto next = [](VideoFrame* cur) -> VideoFrame* {
        return (VideoFrame*)(((uint8_t*)cur) + cur->bytes_of_frame);
    };

    const auto consumed_bytes = [](const VideoFrame* const cur,
                                   const VideoFrame* const end) -> size_t {
        return (uint8_t*)end - (uint8_t*)cur;
    };

    struct clock clock;
    static double time_limit_ms = 20000.0;
    clock_init(&clock);
    clock_shift_ms(&clock, time_limit_ms);
    OK(acquire_start(runtime));
    {
        uint64_t nframes = 0;
        VideoFrame *beg, *end, *cur;
        do {
            struct clock throttle;
            clock_init(&throttle);
            EXPECT(clock_cmp_now(&clock) < 0,
                   "Timeout at %f ms",
                   clock_toc_ms(&clock) + time_limit_ms);
            OK(acquire_map_read(runtime, 0, &beg, &end));
            for (cur = beg; cur < end; cur = next(cur)) {
                LOG("stream %d counting frame w id %d", 0, cur->frame_id);
                CHECK(cur->shape.dims.width == frame_width);
                CHECK(cur->shape.dims.height == frame_height);
                ++nframes;
            }
            {
                uint32_t n = consumed_bytes(beg, end);
                OK(acquire_unmap_read(runtime, 0, n));
                if (n)
                    LOG("stream %d consumed bytes %d", 0, n);
            }
            clock_sleep_ms(&throttle, 100.0f);

            LOG("stream %d expected_frames_per_chunk %d time %f",
                0,
                nframes,
                clock_toc_ms(&clock));
        } while (DeviceState_Running == acquire_get_state(runtime) &&
                 nframes < max_frame_count);

        OK(acquire_map_read(runtime, 0, &beg, &end));
        for (cur = beg; cur < end; cur = next(cur)) {
            LOG("stream %d counting frame w id %d", 0, cur->frame_id);
            CHECK(cur->shape.dims.width == frame_width);
            CHECK(cur->shape.dims.height == frame_height);
            ++nframes;
        }
        {
            uint32_t n = consumed_bytes(beg, end);
            OK(acquire_unmap_read(runtime, 0, n));
            if (n)
                LOG("stream %d consumed bytes %d", 0, n);
        }

        CHECK(nframes == max_frame_count);
    }

    OK(acquire_stop(runtime));
}

void
teardown(AcquireRuntime* runtime)
{
    LOG("Done (OK)");
    acquire_shutdown(runtime);
}

int
main()
{
    auto runtime = acquire_init(reporter);

    setup(runtime);
    acquire(runtime);
    // validation is that it doesn't crash
    teardown(runtime);

    return 0;
}
