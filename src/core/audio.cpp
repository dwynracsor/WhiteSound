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

static void ws_diag_log(const std::string &msg) {
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

    }
}

struct KeepAlive::Impl {
    ma_context context;
    ma_device  device;
    bool       initialized;
    std::atomic<bool> running;
    std::atomic<float> *pLevel;

    KeepAlive *owner = nullptr;
    Settings   settings;
    std::atomic<bool> wantRunning{false};
    std::atomic<bool> needRestart{false};
    std::atomic<bool> stopWatchdog{false};
    std::atomic<bool> selfStop{false};
    std::atomic<bool> autoRouting{false};
    std::mutex        opMtx;
    std::thread       watchdog;

    std::atomic<bool>          autoTune{true};
    std::atomic<float>         baseLevel{0.002f};
    std::atomic<float>         maxLevel{0.02f};
    std::atomic<float>         floorLevel{0.002f};
    std::atomic<int>           noiseMode{0};
    std::atomic<int>           burstDurationMs{200};
    std::atomic<int>           burstIntervalMs{2000};
    std::atomic<std::uint64_t> burstSampleCounter{0};
    std::atomic<std::uint32_t> dropouts{0};
    std::chrono::steady_clock::time_point lastDropoutTime = std::chrono::steady_clock::now();

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

    void startWatchdog() {
        if (watchdog.joinable()) return;
        watchdog = std::thread([this]() {
            int beat = 0;
            while (!stopWatchdog.load()) {
                for (int i = 0; i < 20; ++i) {
                    if (stopWatchdog.load() || needRestart.load()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (stopWatchdog.load()) break;
                if (owner) {
                    owner->verifyAndRestart();
                    owner->autoTuneStep();
                }
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

static inline std::uint32_t xorshift32(std::uint32_t &x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

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

void KeepAlive::CallbackAccess::on_notification(void *pNotificationVoid) {
    if (!pNotificationVoid) return;
    auto *pNotification = static_cast<const ma_device_notification *>(pNotificationVoid);
    if (!pNotification->pDevice) return;
    auto *impl = static_cast<KeepAlive::Impl *>(pNotification->pDevice->pUserData);
    if (!impl) return;

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

static void ws_notification_callback(const ma_device_notification *pNotification) {
    KeepAlive::CallbackAccess::on_notification(
        const_cast<ma_device_notification *>(pNotification));
}

static void data_callback(ma_device *pDevice, void *pOutput, const void * /*pInput*/, ma_uint32 frameCount) {
    if (!pDevice || !pOutput || frameCount == 0) return;

    float level = 0.002f;
    KeepAlive::CallbackAccess::get_level(pDevice, &level);
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    int mode = 0;
    KeepAlive::CallbackAccess::get_noise_mode(pDevice, &mode);

    ma_uint32 channels = pDevice->playback.channels;
    if (channels == 0) channels = 2;

    static thread_local std::uint32_t rng = static_cast<std::uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() ^ std::random_device{}());
    static thread_local PinkState pinkState;

    float *out = static_cast<float *>(pOutput);
    const float invMax = 2.0f / 4294967295.0f;

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
    d.index      = static_cast<int>(list->size());
    d.name       = pInfo->name;
    d.is_default = pInfo->isDefault ? true : false;
    list->push_back(std::move(d));
    return MA_TRUE;
}

std::vector<OutputDevice> KeepAlive::listOutputs() {
    std::vector<OutputDevice> list;
    if (!m_impl) return list;
    std::lock_guard<std::mutex> lk(m_impl->opMtx);

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

    const char *backend = ma_get_backend_name(m_impl->context.backend);
    if (!backend) backend = "WASAPI";
    for (auto &d : list) {
        d.hostapi = backend;
    }
    return list;
}

bool KeepAlive::start(const Settings &s) {
    return configure(s);
}

bool KeepAlive::configure(const Settings &s) {
    if (!m_impl) return false;
    std::lock_guard<std::mutex> lk(m_impl->opMtx);
    m_impl->wantRunning.store(true, std::memory_order_relaxed);
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

bool KeepAlive::configureImpl(const Settings &s) {
    if (!m_impl) return false;
    m_impl->settings = s;
    m_impl->needRestart.store(false, std::memory_order_relaxed);

    if (!m_impl->initialized) {
        ma_result r = ma_context_init(NULL, 0, NULL, &m_impl->context);
        if (r != MA_SUCCESS) {
            setError("ma_context_init failed");
            return false;
        }
        m_impl->initialized = true;
    }

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

    if (m_impl->running.load()) {
        m_impl->selfStop.store(true, std::memory_order_relaxed);
        ma_device_stop(&m_impl->device);
        ma_device_uninit(&m_impl->device);
        m_impl->selfStop.store(false, std::memory_order_relaxed);
        std::memset(&m_impl->device, 0, sizeof(m_impl->device));
        m_impl->running.store(false);
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.pDeviceID = pChosen;
    cfg.playback.format    = ma_format_f32;
    cfg.playback.channels  = 2;
    cfg.sampleRate         = 0;
    cfg.periodSizeInFrames  = 512;
    cfg.periods             = 2;
    cfg.dataCallback       = data_callback;
    cfg.notificationCallback = ws_notification_callback;
    cfg.pUserData          = m_impl;
    if (pChosen == nullptr) {
        cfg.wasapi.noAutoStreamRouting = MA_FALSE;
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

void KeepAlive::verifyAndRestart() {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->opMtx);

    if (!m_impl->wantRunning.load(std::memory_order_relaxed)) {
        m_impl->needRestart.store(false, std::memory_order_relaxed);
        return;
    }

    const bool alive = m_impl->running.load() && ma_device_is_started(&m_impl->device);
    if (alive) {
        if (m_impl->needRestart.exchange(false, std::memory_order_relaxed)) {
            ws_diag_log("watchdog: notification ignored (stream still running)");
        }
        return;
    }

    m_impl->needRestart.store(false, std::memory_order_relaxed);
    ws_diag_log("watchdog: stream not running -> restart");
    configureImpl(m_impl->settings);
    m_impl->dropouts.fetch_add(1, std::memory_order_relaxed);
}

float autoTuneNextLevel(float level, float base, float max,
                        unsigned dropouts, double stableSec, double stableThreshold) {
    static constexpr float kUpStep   = 1.5f;
    static constexpr float kDownStep = 0.97f;
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

static constexpr float kInverseVolumeFactor = 0.7f;

inline float amplitudeToDb(float amplitude) {
    if (amplitude <= 0.0f) return -80.0f;
    float db = 20.0f * std::log10(amplitude);
    if (db < -80.0f) db = -80.0f;
    if (db > 0.0f) db = 0.0f;
    return db;
}

inline float dbToAmplitude(float db) {
    if (db <= -80.0f) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

void KeepAlive::autoTuneStep() {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->opMtx);

    if (!m_impl->autoTune.load(std::memory_order_relaxed) ||
        !m_impl->wantRunning.load(std::memory_order_relaxed)) {
        m_impl->dropouts.store(0, std::memory_order_relaxed);
        m_impl->lastDropoutTime = std::chrono::steady_clock::now();
        return;
    }

    static constexpr double kStableSecs = 20.0;

    const auto now = std::chrono::steady_clock::now();
    const unsigned n = m_impl->dropouts.exchange(0, std::memory_order_relaxed);
    const float base = m_impl->baseLevel.load(std::memory_order_relaxed);
    const float maxL = m_impl->maxLevel.load(std::memory_order_relaxed);
    const float cur  = m_level.load(std::memory_order_relaxed);
    float floorL = m_impl->floorLevel.load(std::memory_order_relaxed);
    if (floorL < base) floorL = base;
    const double stable = std::chrono::duration<double>(now - m_impl->lastDropoutTime).count();

    if (n > 0) m_impl->lastDropoutTime = now;

    const float next = autoTuneNextLevel(cur, floorL, maxL, n, stable, kStableSecs);

    float curDb = amplitudeToDb(cur);
    float inverseTargetDb = -40.0f - curDb;
    inverseTargetDb *= kInverseVolumeFactor;
    if (inverseTargetDb < -80.0f) inverseTargetDb = -80.0f;
    if (inverseTargetDb > 0.0f) inverseTargetDb = 0.0f;
    float inverseTarget = dbToAmplitude(inverseTargetDb);

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
    return WS_AUDIO_BACKEND_NAME;
}

} // namespace ws
