// Unit tests for the portable, pure helpers of the audio engine. No audio device
// is opened: these functions are deliberately side-effect free, which is exactly
// why they were factored out of the live control loop.
//
// Built only when configured with -DWS_BUILD_TESTS=ON; run with `ctest`.

#include "core/keepalive.h"
#include "ws_audio_backend.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void record(bool ok, const char *detail, const char *file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s:%d  %s\n", file, line, detail);
    }
}

void recordClose(float got, float want, const char *detail, const char *file,
                 int line) {
    ++g_checks;
    if (std::fabs(got - want) > 1e-5f) {
        ++g_failures;
        std::printf("FAIL %s:%d  %s  (got %g, want %g)\n", file, line, detail,
                    static_cast<double>(got), static_cast<double>(want));
    }
}

} // namespace

#define CHECK(expr) record((expr), #expr, __FILE__, __LINE__)
#define CHECK_CLOSE(got, want) recordClose((got), (want), #got, __FILE__, __LINE__)

// ---- burstActiveAt ---------------------------------------------------------

static void testBurst() {
    // A non-positive interval or sample rate means "emit continuously".
    CHECK(ws::burstActiveAt(0, 48000, 200, 0));
    CHECK(ws::burstActiveAt(0, 48000, 200, -5));
    CHECK(ws::burstActiveAt(0, 0, 200, 2000));

    // 48 kHz, 200 ms burst every 2000 ms -> 9600 samples on, 86400 off.
    const int rate = 48000, dur = 200, itv = 2000;
    const std::uint64_t interval = 96000ULL;
    const std::uint64_t onSamples = 9600ULL;
    CHECK(ws::burstActiveAt(0, rate, dur, itv));
    CHECK(ws::burstActiveAt(onSamples - 1, rate, dur, itv));
    CHECK(!ws::burstActiveAt(onSamples, rate, dur, itv));
    CHECK(!ws::burstActiveAt(interval - 1, rate, dur, itv));
    // The pattern repeats every interval.
    CHECK(ws::burstActiveAt(interval, rate, dur, itv));
    CHECK(ws::burstActiveAt(interval + onSamples - 1, rate, dur, itv));
    CHECK(!ws::burstActiveAt(interval + onSamples, rate, dur, itv));

    // A duration longer than the interval is clamped, so it is always on.
    CHECK(ws::burstActiveAt(50000, rate, 5000, 1000));

    // Degenerate: sampleRate * intervalMs / 1000 rounds down to zero.
    CHECK(ws::burstActiveAt(7, 1, 50, 100));
}

// ---- autoTuneNextLevel -----------------------------------------------------

static void testAutoTune() {
    const float base = 0.002f, max = 0.02f;

    // No dropouts before the stable window elapses: unchanged.
    CHECK_CLOSE(ws::autoTuneNextLevel(base, base, max, 0, 0.0, 20.0), base);
    CHECK_CLOSE(ws::autoTuneNextLevel(base, base, max, 0, 19.9, 20.0), base);

    // One dropout multiplies the level by 1.5.
    CHECK_CLOSE(ws::autoTuneNextLevel(base, base, max, 1, 0.0, 20.0),
                base * 1.5f);

    // Enough dropouts clamp at max.
    CHECK_CLOSE(ws::autoTuneNextLevel(base, base, max, 100, 0.0, 20.0), max);

    // Stability past the threshold decays by 0.97 per step.
    CHECK_CLOSE(ws::autoTuneNextLevel(0.01f, base, max, 0, 100.0, 20.0),
                0.01f * 0.97f);

    // Decay never crosses the baseline.
    CHECK_CLOSE(ws::autoTuneNextLevel(0.0102f, 0.01f, max, 0, 100.0, 20.0),
                0.01f);

    // max < base is corrected to base.
    CHECK_CLOSE(ws::autoTuneNextLevel(0.001f, 0.01f, 0.005f, 1, 0.0, 20.0),
                0.001f * 1.5f);

    // Negative inputs are clamped to zero.
    CHECK_CLOSE(ws::autoTuneNextLevel(-1.0f, 0.0f, max, 1, 0.0, 20.0), 0.0f);
}

// ---- getBackendName --------------------------------------------------------

static void testBackendName() {
    const std::string name = ws::getBackendName();
    CHECK(!name.empty());
    CHECK(name != "Unknown");
    // Must match the value baked in at configure time.
    CHECK(name == WS_AUDIO_BACKEND_NAME);
}

int main() {
    testBurst();
    testAutoTune();
    testBackendName();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
