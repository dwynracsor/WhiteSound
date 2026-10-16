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

struct Settings {
    int   deviceIndex     = -1;
    float level           = 0.002f;
    bool  autoTune        = true;
    float maxLevel        = 0.02f;
    int   noiseMode       = 0;
    int   burstDurationMs = 200;
    int   burstIntervalMs = 2000;
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
    float effectiveLevel() const;
    bool running() const;

    const std::string &lastError() const { return m_lastError; }

    std::string lastOpenedDeviceName() const;
    int lastSampleRate() const;
    int lastPeriodFrames() const;

    struct Config {
        int  deviceIndex     = -1;
        float level          = 0.002f;
        bool autoTune        = true;
        float maxLevel       = 0.02f;
        int  noiseMode       = 0;
        int  burstDurationMs = 200;
        int  burstIntervalMs = 2000;
        bool enabled         = true;
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
