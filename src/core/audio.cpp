// Define miniaudio implementation ONCE in this translation unit
#define MINIAUDIO_IMPLEMENTATION
#include "../../third_party/miniaudio/miniaudio.h"

#include "keepalive.h"
#include "paths.h"
#include "ws_audio_backend.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

static std::atomic<bool> g_verbose{false};

inline void ws_set_verbose(bool v) {
    g_verbose.store(v);
}

#if defined(NDEBUG) || !defined(_DEBUG)
#define WS_LOG_DEBUG(...) do { if (g_verbose.load()) std::fprintf(stderr, "[dbg] " __VA_ARGS__); } while(0)
#else
#define WS_LOG_DEBUG(fmt, ...) std::fprintf(stderr, "[dbg] " fmt "\n", ##__VA_ARGS__)
#endif

#define WS_LOG_ERROR(fmt, ...) std::fprintf(stderr, "[err] " fmt "\n", ##__VA_ARGS__)

namespace ws {

// Lightweight diagnostic log to ws.log in the per-user config directory
// (see core/paths.h), best-effort and never throwing. Used to confirm which
// endpoint the keep-alive stream opens on and to trace restarts when chasing
// Bluetooth drop-outs.
static void ws_diag_log(const std::string &msg) {
    // Serialize writers: the notification callback (audio thread), the watchdog
    // and the main thread all log here. Concurrent std::ofstream appends to the
    // same file corrupt each other's lines, and can also make the rotation
    // below fail. One mutex keeps every line whole.
    static std::mutex s_logMtx;
    std::lock_guard<std::mutex> lock(s_logMtx);
    try {
        std::error_code ec;
        std::filesystem::path file = platform::logFile();
        std::filesystem::create_directories(file.parent_path(), ec);
        if (std::filesystem::exists(file, ec) &&
            std::filesystem::file_size(file, ec) > 256 * 1024) {
            std::filesystem::remove(file, ec);
        }
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
        std::ofstream out(file, std::ios::app);
        if (out) {
            out << "[" << stamp << "] " << msg << "\n";
        }
    } catch (...) {
        /* diagnostics must never break audio */
    }
}

struct KeepAlive::Impl {
    ma_context context;
    ma_device  device;
    bool       initialized;
    std::atomic<bool> running;
    std::atomic<float> *pLevel; // points to owner m_level

    KeepAlive *owner = nullptr;          // back-pointer for the watchdog
    Settings   settings;                 // last requested settings (for auto-restart)
    std::atomic<bool> wantRunning{false};// the stream should be running
    std::atomic<bool> needRestart{false};// a notification asked for a restart
    std::atomic<bool> stopWatchdog{false};
    std::atomic<bool> selfStop{false};   // we are stopping the device on purpose
    std::atomic<bool> autoRouting{false};// opened on default device (miniaudio may reroute)
    std::mutex        opMtx;             // serializes device init/start/stop
    std::thread       watchdog;

    // Auto-tuner state. dropouts counts stream teardowns (stopped/unlocked/
    // interruption) since the last control step; guarded by opMtx otherwise.
    std::atomic<bool>          autoTune{true};
    std::atomic<float>         baseLevel{0.002f};  // user-requested baseline
    std::atomic<float>         maxLevel{0.02f};    // ceiling for the tuner
    std::atomic<float>         floorLevel{0.002f}; // learned floor (>= baseLevel)
    std::atomic<int>           noiseMode{0};       // 0 = white, 1 = pink, 2 = burst
    std::atomic<int>           burstDurationMs{200};
    std::atomic<int>           burstIntervalMs{2000};
    std::atomic<std::uint64_t> burstSampleCounter{0};
    std::atomic<std::uint32_t> dropouts{0};
    std::chrono::steady_clock::time_point lastDropoutTime = std::chrono::steady_clock::now();

    // Diagnostics (guarded by opMtx)
    std::string actualDeviceName;
    int         actualSampleRate   = 0;
    int         actualPeriodFrames = 0;

    Impl() {
        std::memset(&context, 0, sizeof(context));
        std::memset(&device, 0, sizeof(device));
        initialized = false;
        running     = false;
        pLevel      = nullptr;
    }

    ~Impl() {
        stopWatchdog.store(true);
        if (watchdog.joinable()) {
            watchdog.join();
        }
        if (running.load()) {
            selfStop.store(true, std::memory_order_relaxed);
            ma_device_stop(&device);
        }
        ma_device_uninit(&device);
        selfStop.store(false, std::memory_order_relaxed);
        if (initialized) {
            ma_context_uninit(&context);
        }
        std::memset(&context, 0, sizeof(context));
        std::memset(&device, 0, sizeof(device));
        initialized = false;
        running     = false;
    }

    // Polls the device and restarts it if Windows tore the stream down (e.g. the
    // Bluetooth endpoint was reconfigured when another app grabbed audio).
    void startWatchdog() {
        if (watchdog.joinable()) return;
        watchdog = std::thread([this]() {
            int beat = 0;
            while (!stopWatchdog.load()) {
                for (int i = 0; i < 20; ++i) { // ~1s, waking early if signalled
                    if (stopWatchdog.load() || needRestart.load()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (stopWatchdog.load()) break;
                if (owner) {
                    owner->verifyAndRestart();
                    owner->autoTuneStep();
                }
                // Periodic heartbeat so the timeline in ws.log shows the level
                // that was actually in use when the endpoint cut out.
                if (++beat >= 30 && wantRunning.load(std::memory_order_relaxed) && pLevel) {
                    beat = 0;
                    std::ostringstream ss;
                    const int nmode = noiseMode.load(std::memory_order_relaxed);
                    std::string nstr = "white";
                    if (nmode == 1) nstr = "pink";
                    else if (nmode == 2) nstr = "burst";
                    ss << "heartbeat: level=" << pLevel->load(std::memory_order_relaxed)
                       << " floor=" << floorLevel.load(std::memory_order_relaxed)
                       << " running=" << (running.load() ? 1 : 0)
                       << " noise=" << nstr;
                    ws_diag_log(ss.str());
                }
            }
        });
    }
};

// Xorshift32 for cheap uniform -> white
static inline std::uint32_t xorshift32(std::uint32_t &x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

// Pure core of the burst generator: emit noise for the first `burstDurationMs`
// of every `burstIntervalMs` window, silence otherwise.
bool burstActiveAt(std::uint64_t sampleCounter, int sampleRate,
                   int burstDurationMs, int burstIntervalMs) {
    if (burstIntervalMs <= 0 || sampleRate <= 0) return true;
    const std::uint64_t intervalSamples =
        static_cast<std::uint64_t>(sampleRate) * static_cast<std::uint64_t>(burstIntervalMs) / 1000ULL;
    if (intervalSamples == 0) return true;
    std::uint64_t durSamples =
        static_cast<std::uint64_t>(sampleRate) * static_cast<std::uint64_t>(burstDurationMs) / 1000ULL;
    if (durSamples > intervalSamples) durSamples = intervalSamples;
    const std::uint64_t pos = sampleCounter % intervalSamples;
    return pos < durSamples;
}

// Pink noise generator (Paul Kellett's approximation) state
struct PinkState {
    float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f, b3 = 0.0f, b4 = 0.0f, b5 = 0.0f, b6 = 0.0f;
};

static float generate_pink(float white, PinkState &ps) {
    ps.b0 = 0.99886f * ps.b0 + white * 0.0555179f;
    ps.b1 = 0.99332f * ps.b1 + white * 0.0750759f;
    ps.b2 = 0.96900f * ps.b2 + white * 0.1538520f;
    ps.b3 = 0.86650f * ps.b3 + white * 0.3104856f;
    ps.b4 = 0.55000f * ps.b4 + white * 0.5329522f;
    ps.b5 = -0.7616f * ps.b5 - white * 0.0168980f;
    float pink = ps.b0 + ps.b1 + ps.b2 + ps.b3 + ps.b4 + ps.b5 + ps.b6 + white * 0.5362f;
    ps.b6 = white * 0.115926f;
    return pink;
}

void KeepAlive::CallbackAccess::get_level(void *pDeviceVoid, float *outLevel) {
    if (!pDeviceVoid || !outLevel) return;
    auto *pDevice = static_cast<ma_device *>(pDeviceVoid);
    auto *impl = static_cast<KeepAlive::Impl *>(pDevice->pUserData);
    float level = 0.002f;
    if (impl && impl->pLevel) {
        level = impl->pLevel->load(std::memory_order_relaxed);
    }
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    *outLevel = level;
}

// Read the selected noise generator (0 = white, 1 = pink) from the device.
void KeepAlive::CallbackAccess::get_noise_mode(void *pDeviceVoid, int *outMode) {
    if (!pDeviceVoid || !outMode) return;
    auto *pDevice = static_cast<ma_device *>(pDeviceVoid);
    auto *impl = static_cast<KeepAlive::Impl *>(pDevice->pUserData);
    int mode = 0;
    if (impl) {
        mode = impl->noiseMode.load(std::memory_order_relaxed);
    }
    if (mode != 0 && mode != 1 && mode != 2) mode = 0;
    *outMode = mode;
}

bool KeepAlive::CallbackAccess::next_burst_emit(void *pDeviceVoid, unsigned sampleRate) {
    if (!pDeviceVoid) return true;
    auto *pDevice = static_cast<ma_device *>(pDeviceVoid);
    auto *impl = static_cast<KeepAlive::Impl *>(pDevice->pUserData);
    if (!impl) return true;
    const int dur = impl->burstDurationMs.load(std::memory_order_relaxed);
    const int itv = impl->burstIntervalMs.load(std::memory_order_relaxed);
    const std::uint64_t cnt = impl->burstSampleCounter.fetch_add(1, std::memory_order_relaxed);
    return burstActiveAt(cnt, static_cast<int>(sampleRate), dur, itv);
}

// miniaudio notification handler: Windows tells us when the endpoint was
// rerouted, unlocked, or the stream was stopped. Flag a restart so the
// watchdog brings the white noise back (covers browser grabbing the device).
void KeepAlive::CallbackAccess::on_notification(void *pNotificationVoid) {
    if (!pNotificationVoid) return;
    auto *pNotification = static_cast<const ma_device_notification *>(pNotificationVoid);
    if (!pNotification->pDevice) return;
    auto *impl = static_cast<KeepAlive::Impl *>(pNotification->pDevice->pUserData);
    if (!impl) return;

    // A notification raised by our own stop/uninit is not an external dropout.
    if (impl->selfStop.load(std::memory_order_relaxed)) {
        return;
    }

    const char *name = "?";
    switch (pNotification->type) {
        case ma_device_notification_type_started:          name = "started"; break;
        case ma_device_notification_type_stopped:          name = "stopped"; break;
        case ma_device_notification_type_rerouted:         name = "rerouted"; break;
        case ma_device_notification_type_interruption_began: name = "interruption_began"; break;
        case ma_device_notification_type_interruption_ended: name = "interruption_ended"; break;
        case ma_device_notification_type_unlocked:         name = "unlocked"; break;
        default: break;
    }

    switch (pNotification->type) {
        case ma_device_notification_type_stopped:
        case ma_device_notification_type_unlocked:
        case ma_device_notification_type_interruption_ended:
            ws_diag_log(std::string("notification: ") + name + " -> restart");
            impl->needRestart.store(true, std::memory_order_relaxed);
            break;
        case ma_device_notification_type_rerouted:
            // When miniaudio owns auto-routing (default device) it already
            // rebuilt the stream; restarting again here would add a click.
            if (!impl->autoRouting.load(std::memory_order_relaxed)) {
                ws_diag_log("notification: rerouted -> restart");
                impl->needRestart.store(true, std::memory_order_relaxed);
            } else {
                ws_diag_log("notification: rerouted (handled by miniaudio)");
            }
            break;
        default:
            ws_diag_log(std::string("notification: ") + name);
            break;
    }
}

// Free-function trampoline with the signature miniaudio expects.
static void ws_notification_callback(const ma_device_notification *pNotification) {
    KeepAlive::CallbackAccess::on_notification(
        const_cast<ma_device_notification *>(pNotification));
}

// Data callback: generate white (default) or pink noise scaled by level
static void data_callback(ma_device *pDevice, void *pOutput, const void * /*pInput*/, ma_uint32 frameCount) {
    if (!pDevice || !pOutput || frameCount == 0) return;

    float level = 0.002f;
    KeepAlive::CallbackAccess::get_level(pDevice, &level);
    // Clamp
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    int mode = 0;
    KeepAlive::CallbackAccess::get_noise_mode(pDevice, &mode);

    ma_uint32 channels = pDevice->playback.channels;
    if (channels == 0) channels = 2;

    // xorshift state per call is fine; use thread_local
    static thread_local std::uint32_t rng = static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() ^ std::random_device{}());
    static thread_local PinkState pinkState;

    float *out = static_cast<float *>(pOutput);
    const float invMax = 2.0f / 4294967295.0f; // scale [0, UINT32_MAX] to [-1,1)

    const ma_uint32 sampleRate = pDevice->sampleRate > 0 ? pDevice->sampleRate : 44100;

    for (ma_uint32 f = 0; f < frameCount; ++f) {
        bool emitNoise = true;
        if (mode == 2) {
            emitNoise = KeepAlive::CallbackAccess::next_burst_emit(pDevice, sampleRate);
        }

        float val = 0.0f;
        if (emitNoise) {
            const float white = (static_cast<float>(static_cast<int32_t>(xorshift32(rng))) * invMax);
            const float src = (mode == 1) ? generate_pink(white, pinkState) : white;
            val = src * level;
            if (val > 1.0f) val = 1.0f;
            if (val < -1.0f) val = -1.0f;
        } else {
            val = 0.0f;
        }
        for (ma_uint32 ch = 0; ch < channels; ++ch) {
            *out++ = val;
        }
    }
}

KeepAlive::KeepAlive() : m_impl(new Impl), m_level(0.002f) {
    m_impl->pLevel = &m_level;
    m_impl->owner  = this;
    m_impl->startWatchdog();
}

KeepAlive::~KeepAlive() {
    stop();
    delete m_impl;
    m_impl = nullptr;
}

static ma_bool32 enum_playback(ma_context * /*pContext*/, ma_device_type deviceType,
                                const ma_device_info *pInfo, void *pUserData) {
    if (deviceType != ma_device_type_playback) return MA_TRUE;
    auto *list = static_cast<std::vector<OutputDevice> *>(pUserData);
    OutputDevice d;
    // index is position in list
    d.index      = static_cast<int>(list->size());
    d.name       = pInfo->name;
    // hostapi is filled in by listOutputs() once the backend is known.
    d.is_default = pInfo->isDefault ? true : false;
    list->push_back(std::move(d));
    return MA_TRUE;
}

std::vector<OutputDevice> KeepAlive::listOutputs() {
    std::vector<OutputDevice> list;
    if (!m_impl) return list;
    std::lock_guard<std::mutex> lk(m_impl->opMtx);

    // Ensure context initialized
    if (!m_impl->initialized) {
        ma_result r = ma_context_init(NULL, 0, NULL, &m_impl->context);
        if (r != MA_SUCCESS) {
            setError("ma_context_init failed");
            return list;
        }
        m_impl->initialized = true;
    }

    ma_result r = ma_context_enumerate_devices(&m_impl->context, enum_playback, &list);
    if (r != MA_SUCCESS) {
        setError("ma_context_enumerate_devices failed");
        return list;
    }

    // set hostapi from backend name
    const char *backend = ma_get_backend_name(m_impl->context.backend);
    if (!backend) backend = "WASAPI";
    for (auto &d : list) {
        d.hostapi = backend;
    }
    return list;
}

bool KeepAlive::start(const Settings &s) {
    // Pass the settings through untouched: deviceIndex < 0 means "system
    // default" and is opened with pDeviceID = NULL so miniaudio/WASAPI can
    // follow the default endpoint (e.g. a Bluetooth speaker) automatically.
    return configure(s);
}

bool KeepAlive::configure(const Settings &s) {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lk(m_impl->opMtx);
    m_impl->wantRunning.store(true, std::memory_order_relaxed);
    // Apply the user-requested baseline and (re)seed the auto-tuner. Internal
    // restarts (verifyAndRestart) call configureImpl() directly and must keep
    // the level the tuner has climbed to, so that seeding lives here only.
    const float startLevel = s.level >= 0.0f ? s.level : 0.002f;
    m_level.store(startLevel, std::memory_order_relaxed);
    m_impl->baseLevel.store(startLevel, std::memory_order_relaxed);
    m_impl->floorLevel.store(startLevel, std::memory_order_relaxed);
    m_impl->autoTune.store(s.autoTune, std::memory_order_relaxed);
    m_impl->maxLevel.store(s.maxLevel > 0.0f ? s.maxLevel : 0.02f, std::memory_order_relaxed);
    int nm = s.noiseMode;
    if (nm != 0 && nm != 1 && nm != 2) nm = 0;
    m_impl->noiseMode.store(nm, std::memory_order_relaxed);
    m_impl->burstDurationMs.store(s.burstDurationMs > 0 ? s.burstDurationMs : 200, std::memory_order_relaxed);
    m_impl->burstIntervalMs.store(s.burstIntervalMs > 0 ? s.burstIntervalMs : 2000, std::memory_order_relaxed);
    m_impl->burstSampleCounter.store(0, std::memory_order_relaxed);
    m_impl->dropouts.store(0, std::memory_order_relaxed);
    m_impl->lastDropoutTime = std::chrono::steady_clock::now();
    return configureImpl(s);
}

// Must be called with m_impl->opMtx held (configure() and verifyAndRestart()).
bool KeepAlive::configureImpl(const Settings &s) {
    if (!m_impl) return false;
    m_impl->settings = s;
    m_impl->needRestart.store(false, std::memory_order_relaxed);

    // Ensure context
    if (!m_impl->initialized) {
        ma_result r = ma_context_init(NULL, 0, NULL, &m_impl->context);
        if (r != MA_SUCCESS) {
            setError("ma_context_init failed");
            return false;
        }
        m_impl->initialized = true;
    }

    // Get devices
    ma_device_info *pPlayback = nullptr;
    ma_uint32 playbackCount   = 0;
    ma_device_info *pCapture  = nullptr;
    ma_uint32 captureCount    = 0;
    ma_result r = ma_context_get_devices(&m_impl->context, &pPlayback, &playbackCount, &pCapture, &captureCount);
    if (r != MA_SUCCESS) {
        setError("ma_context_get_devices failed");
        return false;
    }
    WS_LOG_DEBUG("got devices: playback=%u, capture=%u, pPlayback=%p, pCapture=%p",
                 playbackCount, captureCount, (void*)pPlayback, (void*)pCapture);

    // Choose device id. When no explicit index is requested we leave the id NULL
    // so miniaudio follows the *default* device and enables WASAPI auto stream
    // routing (it can re-route to the new default endpoint on its own).
    const ma_device_id *pChosen = nullptr;
    if (s.deviceIndex >= 0 && s.deviceIndex < static_cast<int>(playbackCount)) {
        pChosen = &pPlayback[s.deviceIndex].id;
    }
    m_impl->autoRouting.store(pChosen == nullptr, std::memory_order_relaxed);

    {
        std::ostringstream ss;
        ss << "configure: requested device=" << (pChosen ? std::to_string(s.deviceIndex) : std::string("default"))
           << " level=" << s.level;
        ws_diag_log(ss.str());
    }

    // If running and device different? But easier: stop first if running
    if (m_impl->running.load()) {
        m_impl->selfStop.store(true, std::memory_order_relaxed);
        ma_device_stop(&m_impl->device);
        ma_device_uninit(&m_impl->device);
        m_impl->selfStop.store(false, std::memory_order_relaxed);
        std::memset(&m_impl->device, 0, sizeof(m_impl->device));
        m_impl->running.store(false);
    }

    // Config
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.pDeviceID = pChosen;
    cfg.playback.format    = ma_format_f32;
    cfg.playback.channels  = 2;
    // Use 0 to let miniaudio pick the device's native sample rate. Forcing a
    // fixed rate here can make a Bluetooth endpoint renegotiate its format.
    cfg.sampleRate         = 0;
    cfg.periodSizeInFrames  = 512;
    cfg.periods             = 2;
    cfg.dataCallback       = data_callback;
    cfg.notificationCallback = ws_notification_callback;
    cfg.pUserData          = m_impl;
    if (pChosen == nullptr) {
        // Follow default endpoint on WASAPI reroute (important for BT A2DP)
        cfg.wasapi.noAutoStreamRouting = MA_FALSE;
        // Avoid auto SRC changes that can trigger endpoint reinit
        cfg.wasapi.noAutoConvertSRC = MA_TRUE;
    }

    WS_LOG_DEBUG("ma_device_init...");
    r = ma_device_init(&m_impl->context, &cfg, &m_impl->device);
    WS_LOG_DEBUG("ma_device_init returned %d", r);
    if (r != MA_SUCCESS) {
        std::ostringstream ss;
        ss << "ma_device_init failed: " << r;
        setError(ss.str());
        ws_diag_log("ma_device_init FAILED: " + std::to_string(r));
        return false;
    }

    // Record the endpoint WASAPI actually opened (may differ from the request
    // when running in default-device mode). This is what the UI shows so the
    // user can confirm the keep-alive is going to the Bluetooth speaker.
    m_impl->actualDeviceName   = m_impl->device.playback.name;
    m_impl->actualSampleRate   = static_cast<int>(m_impl->device.sampleRate);
    m_impl->actualPeriodFrames = static_cast<int>(m_impl->device.playback.internalPeriodSizeInFrames);
    {
        std::ostringstream ss;
        ss << "opened device=\"" << m_impl->actualDeviceName << "\""
           << " rate=" << m_impl->actualSampleRate
           << " channels=" << m_impl->device.playback.channels
           << " periodFrames=" << m_impl->actualPeriodFrames
           << " autoRouting=" << (pChosen == nullptr ? "yes" : "no");
        ws_diag_log(ss.str());
    }

    // If device supports different channels, respect? but we send interleaved to channels
    // After init, device.playback.channels is actual
    WS_LOG_DEBUG("ma_device_start...");
    r = ma_device_start(&m_impl->device);
    WS_LOG_DEBUG("ma_device_start returned %d", r);
    WS_LOG_DEBUG("after ma_device_start call");
    if (r != MA_SUCCESS) {
        std::ostringstream ss;
        ss << "ma_device_start failed: " << r;
        setError(ss.str());
        ws_diag_log("ma_device_start FAILED: " + std::to_string(r));
        ma_device_uninit(&m_impl->device);
        std::memset(&m_impl->device, 0, sizeof(m_impl->device));
        return false;
    }
    WS_LOG_DEBUG("past error check");

    ws_diag_log("stream started OK");
    m_impl->running.store(true);
    WS_LOG_DEBUG("set running=true");
    WS_LOG_DEBUG("configure returning true");
    return true;
}

// Called by the watchdog thread roughly every second. Rebuilds the stream only
// when it is actually gone. A mere notification is NOT a reason to tear down a
// running stream: on Bluetooth the stop/start churn itself becomes a continuous
// stutter (and Windows keeps re-sending such notifications while a browser or
// Spotify shares the endpoint).
void KeepAlive::verifyAndRestart() {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->opMtx);

    if (!m_impl->wantRunning.load(std::memory_order_relaxed)) {
        m_impl->needRestart.store(false, std::memory_order_relaxed);
        return;
    }

    const bool alive = m_impl->running.load() && ma_device_is_started(&m_impl->device);
    if (alive) {
        // Stream healthy: drop any pending notification without restarting it.
        if (m_impl->needRestart.exchange(false, std::memory_order_relaxed)) {
            ws_diag_log("watchdog: notification ignored (stream still running)");
        }
        return;
    }

    // Stream is gone: rebuild it.
    m_impl->needRestart.store(false, std::memory_order_relaxed);
    ws_diag_log("watchdog: stream not running -> restart");
    configureImpl(m_impl->settings);
    // A real teardown is the symptom we treat as a drop-out; the auto-tuner
    // nudges the level up so the endpoint stops sleeping.
    m_impl->dropouts.fetch_add(1, std::memory_order_relaxed);
}

// Pure, testable core of the auto-tuner. dropouts > 0 raises the level toward
// `max`; otherwise, once `stableSec` has elapsed, it decays toward `base`.
float autoTuneNextLevel(float level, float base, float max,
                        unsigned dropouts, double stableSec, double stableThreshold) {
    static constexpr float kUpStep   = 1.5f;   // +~3.5 dB per dropout
    static constexpr float kDownStep = 0.97f;  // -0.26 dB per stable second
    static constexpr float kEps      = 1e-4f;
    if (level < 0.0f) level = 0.0f;
    if (base < 0.0f) base = 0.0f;
    if (max < base) max = base;
    if (dropouts > 0) {
        float next = level;
        for (unsigned i = 0; i < dropouts; ++i) next = std::min(max, next * kUpStep);
        return next;
    }
    if (stableSec >= stableThreshold && level > base + kEps) {
        float next = level * kDownStep;
        if (next < base) next = base;
        return next;
    }
    return level;
}

// Inverse volume factor: controls how strongly the auto-tune responds to
// volume changes. 1.0 = full inverse relationship, 0.0 = no effect.
static constexpr float kInverseVolumeFactor = 0.7f;

// Convert amplitude to dBFS (clamped to valid range)
inline float amplitudeToDb(float amplitude) {
    if (amplitude <= 0.0f) return -80.0f;
    float db = 20.0f * std::log10(amplitude);
    if (db < -80.0f) db = -80.0f;
    if (db > 0.0f) db = 0.0f;
    return db;
}

// Convert dBFS to amplitude
inline float dbToAmplitude(float db) {
    if (db <= -80.0f) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

// Auto-tune control loop (runs on the watchdog thread ~1/s). AIMD: on each
// detected dropout the level is multiplied up to maxLevel; after a long stable
// window it decays back toward the user baseline to keep the hiss minimal.
// Additionally, the level is inversely adjusted based on the current volume
// so that when output volume is high, noise is minimized, and vice versa.
void KeepAlive::autoTuneStep() {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->opMtx);

    if (!m_impl->autoTune.load(std::memory_order_relaxed) ||
        !m_impl->wantRunning.load(std::memory_order_relaxed)) {
        m_impl->dropouts.store(0, std::memory_order_relaxed);
        m_impl->lastDropoutTime = std::chrono::steady_clock::now();
        return;
    }

    static constexpr double kStableSecs = 20.0;   // quiet window before decaying

    const auto now = std::chrono::steady_clock::now();
    const unsigned n = m_impl->dropouts.exchange(0, std::memory_order_relaxed);
    const float base = m_impl->baseLevel.load(std::memory_order_relaxed);
    const float maxL = m_impl->maxLevel.load(std::memory_order_relaxed);
    const float cur  = m_level.load(std::memory_order_relaxed);
    // The tuner may never decay below the learned floor: the level that was
    // actually needed to survive a dropout. Decaying all the way back to the
    // (possibly inaudible) baseline is what produced the periodic cut-outs.
    float floorL = m_impl->floorLevel.load(std::memory_order_relaxed);
    if (floorL < base) floorL = base;
    const double stable = std::chrono::duration<double>(now - m_impl->lastDropoutTime).count();

    if (n > 0) m_impl->lastDropoutTime = now;

    // Standard auto-tune step based on drop-outs
    const float next = autoTuneNextLevel(cur, floorL, maxL, n, stable, kStableSecs);

    // --- Inverse volume adjustment ---
    // When the noise level setting is high (closer to 0 dBFS), the inverse
    // target is low (more quiet). When the noise level setting is low, the
    // inverse target allows a slightly higher noise floor.
    // Formula: inverseTargetDb = -40 - currentDb, clamped to [-80, 0] range.
    float curDb = amplitudeToDb(cur);
    // Target: when output is at 0 dBFS, noise should be at -40 dBFS max.
    // When output is at -80 dBFS, noise can be at -80 dBFS (floor).
    // We use a factor so it's not extremely aggressive.
    float inverseTargetDb = -40.0f - curDb;
    // Scale factor: 0.5 means half the inverse, 1.0 means full inverse
    inverseTargetDb *= kInverseVolumeFactor;
    // Clamp to valid dBFS range
    if (inverseTargetDb < -80.0f) inverseTargetDb = -80.0f;
    if (inverseTargetDb > 0.0f) inverseTargetDb = 0.0f;
    float inverseTarget = dbToAmplitude(inverseTargetDb);

    // The final level is the standard auto-tune next, but never above the
    // inverse volume target (so noise never gets too loud when volume is high)
    float finalNext = next;
    if (inverseTarget < finalNext) {
        finalNext = inverseTarget;
    }

    if (n > 0 && finalNext > floorL) {
        m_impl->floorLevel.store(finalNext, std::memory_order_relaxed);
    }
    if (std::fabs(finalNext - cur) > 1e-4f) {
        m_level.store(finalNext, std::memory_order_relaxed);
        std::ostringstream ss;
        if (n > 0) {
            ss << "autotune: " << n << " drop-out(s) -> level " << cur << " -> " << finalNext
               << " (invtarget=" << inverseTargetDb << "dBFS)";
        } else {
            ss << "autotune: stable -> level " << cur << " -> " << finalNext
               << " (invtarget=" << inverseTargetDb << "dBFS)";
        }
        ws_diag_log(ss.str());
    }
}

void KeepAlive::stop() {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->opMtx);
    m_impl->wantRunning.store(false, std::memory_order_relaxed);
    m_impl->needRestart.store(false, std::memory_order_relaxed);
    if (m_impl->running.load()) {
        m_impl->selfStop.store(true, std::memory_order_relaxed);
        ma_device_stop(&m_impl->device);
        ma_device_uninit(&m_impl->device);
        m_impl->selfStop.store(false, std::memory_order_relaxed);
        std::memset(&m_impl->device, 0, sizeof(m_impl->device));
        m_impl->running.store(false);
        ws_diag_log("stream stopped");
    }
}

std::string KeepAlive::lastOpenedDeviceName() const {
    if (!m_impl) return {};
    std::lock_guard<std::mutex> lk(m_impl->opMtx);
    return m_impl->actualDeviceName;
}

int KeepAlive::lastSampleRate() const {
    if (!m_impl) return 0;
    std::lock_guard<std::mutex> lk(m_impl->opMtx);
    return m_impl->actualSampleRate;
}

int KeepAlive::lastPeriodFrames() const {
    if (!m_impl) return 0;
    std::lock_guard<std::mutex> lk(m_impl->opMtx);
    return m_impl->actualPeriodFrames;
}

void KeepAlive::setLevel(float level) {
    const float v = level >= 0.0f ? level : 0.002f;
    if (m_impl) {
        std::lock_guard<std::mutex> lk(m_impl->opMtx);
        m_impl->baseLevel.store(v, std::memory_order_relaxed);
        m_impl->floorLevel.store(v, std::memory_order_relaxed);
        m_impl->dropouts.store(0, std::memory_order_relaxed);
        m_impl->lastDropoutTime = std::chrono::steady_clock::now();
    }
    m_level.store(v, std::memory_order_relaxed);
}

void KeepAlive::setAutoTune(bool enabled) {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->opMtx);
    const bool was = m_impl->autoTune.exchange(enabled, std::memory_order_relaxed);
    if (enabled && !was) {
        // Start tuning from whatever level is currently applied.
        const float cur = m_level.load(std::memory_order_relaxed);
        m_impl->baseLevel.store(cur, std::memory_order_relaxed);
        m_impl->floorLevel.store(cur, std::memory_order_relaxed);
    }
    m_impl->dropouts.store(0, std::memory_order_relaxed);
    m_impl->lastDropoutTime = std::chrono::steady_clock::now();
}

bool KeepAlive::autoTuneEnabled() const {
    return m_impl ? m_impl->autoTune.load(std::memory_order_relaxed) : false;
}

void KeepAlive::setNoiseMode(int mode) {
    if (!m_impl) return;
    if (mode != 0 && mode != 1 && mode != 2) mode = 0;
    m_impl->noiseMode.store(mode, std::memory_order_relaxed);
}

void KeepAlive::setBurstDuration(int ms) {
    if (!m_impl) return;
    if (ms < 10) ms = 10;
    if (ms > 10000) ms = 10000;
    m_impl->burstDurationMs.store(ms, std::memory_order_relaxed);
    m_impl->burstSampleCounter.store(0, std::memory_order_relaxed);
}

void KeepAlive::setBurstInterval(int ms) {
    if (!m_impl) return;
    if (ms < 100) ms = 100;
    if (ms > 60000) ms = 60000;
    m_impl->burstIntervalMs.store(ms, std::memory_order_relaxed);
    m_impl->burstSampleCounter.store(0, std::memory_order_relaxed);
}

int KeepAlive::burstDurationMs() const {
    return m_impl ? m_impl->burstDurationMs.load(std::memory_order_relaxed) : 200;
}

int KeepAlive::burstIntervalMs() const {
    return m_impl ? m_impl->burstIntervalMs.load(std::memory_order_relaxed) : 2000;
}

int KeepAlive::noiseMode() const {
    return m_impl ? m_impl->noiseMode.load(std::memory_order_relaxed) : 0;
}

float KeepAlive::effectiveLevel() const {
    return m_level.load(std::memory_order_relaxed);
}

bool KeepAlive::running() const {
    return m_impl ? m_impl->running.load() : false;
}

void KeepAlive::setError(const std::string &e) {
    std::lock_guard<std::mutex> lk(m_errMtx);
    m_lastError = e;
}

void setVerbose(bool enabled) {
    ws_set_verbose(enabled);
}

std::string getBackendName() {
    // Set at configure time from the backend miniaudio was built with (see
    // CMakeLists.txt). MA_API cannot be used for this: it is miniaudio's
    // linkage attribute, not a backend selector.
    return WS_AUDIO_BACKEND_NAME;
}

} // namespace ws
