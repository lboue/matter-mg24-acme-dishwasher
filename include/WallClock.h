#pragma once

#include <app/clusters/time-synchronization-server/DefaultTimeSyncDelegate.h>
#include <system/SystemLayer.h>

/**
 * The ESP32 version got wall-clock time from SNTP. There is no SNTP over Thread, and the Silabs
 * FreeRTOS platform does not implement GetClock_RealTime(), so instead we keep our own offset from
 * the monotonic clock, set by the Time Synchronization cluster. That happens either when a
 * controller sends SetUTCTime (`chip-tool timesynchronization set-utctime ...`), or when the
 * cluster reads UTCTime from the trusted time source a controller set with SetTrustedTimeSource
 * (the Time Sync Client feature).
 */
class WallClock : public chip::app::Clusters::TimeSynchronization::DefaultTimeSyncDelegate
{
public:
    static WallClock & Instance() { return sInstance; }

    void Init();

    /** Current time in seconds since the Matter epoch (2000-01-01), as used by the DEM forecast. */
    bool GetMatterEpochSeconds(uint32_t & seconds) const;

    bool IsSynced() const { return mSynced; }

    void UTCTimeAvailabilityChanged(uint64_t matterEpochMicros) override;

private:
    static WallClock sInstance;

    static void RetryTimeSync(chip::System::Layer * layer, void * context);

    bool mSynced                    = false;
    uint64_t mMatterEpochMicros     = 0;
    uint64_t mMonotonicMicrosAtSync = 0;
};
