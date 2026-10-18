#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

namespace ws {

struct OutputDevice {
    int         index;
    std::string name;
    std::string hostapi;
    bool        is_default;
};

// Noise shapes. 0-2 are the original broadband modes (they keep a sleeping
// amplifier awake). 3-5 are band-limited probes: a speaker whose internal DSP
// gates or limits a signal (e.g. it drops the first syllable of every phrase)
// reacts to one specific part of the spectrum, and these modes let the user
// find out which one by ear.
enum NoiseMode : int {
    NoiseWhite      = 0,  // uniform noise, 0 Hz - Nyquist (default)
    NoisePink       = 1,  // pink noise, more energy in the low end
    NoiseBurst      = 2,  // short noise pulses every burstIntervalMs
    NoiseSpeechBand = 3,  // band-passed noise, 300 Hz - 3.4 kHz (voice band)
    NoiseLowBand    = 4,  // low-passed noise, below ~150 Hz (sub-bass)
    NoiseTone       = 5,  // continuous sine at toneHz
};

constexpr int kNoiseModeCount = 6;

int clampNoiseMode(int mode);

// Highest slider level that keeps the shaped output below the output headroom
// for the given mode. Above it the engine limits the gain, so the fill would be
// squashed (and, in the low band, distorted).
float noiseModeLevelCeiling(int mode);

struct Settings {
    int   deviceIndex     = -1;
    float level           = 0.002f;
    bool  autoTune        = true;
    float maxLevel        = 0.06f;
    int   noiseMode       = NoiseWhite;
    int   burstDurationMs = 200;
    int   burstIntervalMs = 2000;
    int   toneHz          = 1000;  // used by NoiseTone
};

class KeepAlive {
public:
    KeepAlive();
    ~KeepAlive();

    std::vector<OutputDevice> listOutputs();

    bool start(const Settings &s);
    void stop();
    bool configure(const Settings &s);
    void setLevel(float level);
    void setAutoTune(bool enabled);
    bool autoTuneEnabled() const;
    void setNoiseMode(int mode);
    int noiseMode() const;
    void setBurstDuration(int ms);
    void setBurstInterval(int ms);
    int  burstDurationMs() const;
    int  burstIntervalMs() const;
    void setToneHz(int hz);
    int  toneHz() const;
    float effectiveLevel() const;
    bool running() const;

    const std::string &lastError() const { return m_lastError; }

    std::string lastOpenedDeviceName() const;
    int lastSampleRate() const;
    int lastPeriodFrames() const;

    struct Config {
        int   deviceIndex     = -1;
        float level           = 0.002f;
        bool  autoTune        = true;
        float maxLevel        = 0.06f;
        int   noiseMode       = NoiseWhite;
        int   burstDurationMs = 200;
        int   burstIntervalMs = 2000;
        int   toneHz          = 1000;
        bool  enabled         = true;
        // Level that was tuned for each noise mode, so switching mode restores
        // the level that worked there (0 = never tuned; use the current level).
        float levelByMode[kNoiseModeCount] = {};
    };

private:
    struct Impl;
    Impl *m_impl;
    mutable std::mutex m_errMtx;
    std::string m_lastError;
    std::atomic<float> m_level;

    void setError(const std::string &e);

    bool configureImpl(const Settings &s);
    void verifyAndRestart();
    void autoTuneStep();

public:
    struct CallbackAccess {
        static void get_level(void *pDevice, float *outLevel);
        static void get_noise_mode(void *pDevice, int *outMode);
        static void get_tone_hz(void *pDevice, int *outHz);
        static void on_notification(void *pNotification);
        static bool next_burst_emit(void *pDevice, unsigned sampleRate);
    };
};

float autoTuneNextLevel(float level, float base, float max,
                        unsigned dropouts, double stableSec, double stableThreshold);

bool burstActiveAt(std::uint64_t sampleCounter, int sampleRate,
                   int burstDurationMs, int burstIntervalMs);

void setVerbose(bool enabled);

std::string getBackendName();

} // namespace ws
