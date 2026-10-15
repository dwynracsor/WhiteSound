#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

namespace ws {

struct OutputDevice {
    int         index;      // 0-based in our enumeration list
    std::string name;
    std::string hostapi;
    bool        is_default; // best-effort
};

struct Settings {
    int   deviceIndex     = -1;      // -1 = default
    float level           = 0.002f;   // 0.0 - 1.0 approx (starting / user level)
    bool  autoTune        = true;     // raise level automatically on stream drop-outs
    float maxLevel        = 0.02f;    // upper bound the auto-tuner may reach
    int   noiseMode       = 0;        // 0 = white (default), 1 = pink, 2 = burst
    // Burst parameters (noiseMode == 2)
    int   burstDurationMs = 200;      // duration of noise burst
    int   burstIntervalMs = 2000;     // interval between bursts
};

class KeepAlive {
public:
    KeepAlive();
    ~KeepAlive();

    // List playback (output) devices
    std::vector<OutputDevice> listOutputs();

    // Start with given settings. -1 uses default.
    bool start(const Settings &s);
    // Stop playback
    void stop();
    // Configure: may restart stream if device changes; level changes live if running
    bool configure(const Settings &s);
    // Change level live (thread-safe). Also resets the auto-tuner baseline.
    void setLevel(float level);
    // Enable/disable the automatic level tuner (thread-safe).
    void setAutoTune(bool enabled);
    // Whether the automatic level tuner is enabled.
    bool autoTuneEnabled() const;
    // Select the noise generator live (thread-safe). 0 = white, 1 = pink, 2 = burst.
    void setNoiseMode(int mode);
    // Currently selected noise generator (0 = white, 1 = pink, 2 = burst).
    int noiseMode() const;
    void setBurstDuration(int ms);
    void setBurstInterval(int ms);
    int  burstDurationMs() const;
    int  burstIntervalMs() const;
    // The level currently applied to the stream. Equals Settings::level unless
    // the auto-tuner has raised it above the baseline.
    float effectiveLevel() const;
    // Check state
    bool running() const;

    // Optional: get last error string
    const std::string &lastError() const { return m_lastError; }

    // Diagnostics: the playback device the stream is actually open on
    // (empty until a stream has been started). Reflects the real endpoint
    // chosen by WASAPI when running in "default device" mode.
    std::string lastOpenedDeviceName() const;
    int lastSampleRate() const;
    int lastPeriodFrames() const;

    // The on-disk configuration record. The engine only consumes the audio
    // fields (via Settings); it is defined here so the host keeps a single
    // definition of the persisted schema.
    struct Config {
        int  deviceIndex     = -1;
        float level          = 0.002f;
        bool autoTune        = true;
        float maxLevel       = 0.02f;
        int  noiseMode       = 0;        // 0 = white (default), 1 = pink, 2 = burst
        int  burstDurationMs = 200;
        int  burstIntervalMs = 2000;
        bool enabled         = true;     // emit as soon as the app starts
    };

private:
    // Forward decl of impl to avoid including miniaudio in header
    struct Impl;
    Impl *m_impl;
    mutable std::mutex m_errMtx;
    std::string m_lastError;
    std::atomic<float> m_level;

    void setError(const std::string &e);

    // Internal helpers (defined in audio.cpp). configureImpl() expects Impl::opMtx
    // to be held; verifyAndRestart() takes it itself (called by the watchdog).
    bool configureImpl(const Settings &s);
    void verifyAndRestart();
    // One step of the auto-tuner control loop (called by the watchdog ~1/s).
    void autoTuneStep();

public:
    // Callback access
    struct CallbackAccess {
        static void get_level(void *pDevice, float *outLevel);
        static void get_noise_mode(void *pDevice, int *outMode);
        static void on_notification(void *pNotification);
        // Advances the burst sample counter and returns whether the current
        // sample should emit noise (call once per output frame in burst mode).
        static bool next_burst_emit(void *pDevice, unsigned sampleRate);
    };
};

// Pure helper implementing one step of the auto-tuner (exposed for unit
// testing; the live control loop lives in audio.cpp). If `dropouts` > 0 the
// level is raised toward `max`; otherwise, once `stableSec` has elapsed since
// the last dropout (>= `stableThreshold`), it decays toward `base`.
float autoTuneNextLevel(float level, float base, float max,
                        unsigned dropouts, double stableSec, double stableThreshold);

// Pure helper: whether the burst generator should emit noise for the given
// 0-based sample counter. A burst lasts `burstDurationMs` at the start of every
// `burstIntervalMs` window. Returns true (continuous) if the interval is not
// positive.
bool burstActiveAt(std::uint64_t sampleCounter, int sampleRate,
                   int burstDurationMs, int burstIntervalMs);

// Global verbosity control (for debug logging)
void setVerbose(bool enabled);

// Get the name of the audio backend this build uses (WASAPI, ALSA, PulseAudio,
// JACK, CoreAudio, ...), as configured in CMakeLists.txt.
std::string getBackendName();

} // namespace ws
