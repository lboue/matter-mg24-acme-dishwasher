#include "WallClock.h"

#include <app/clusters/time-synchronization-server/CodegenIntegration.h>
#include <app/clusters/time-synchronization-server/TimeSynchronizationCluster.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>
#include <system/SystemClock.h>

using namespace chip;

WallClock WallClock::sInstance;

namespace {
constexpr System::Clock::Seconds32 kRetryInterval(120);
} // namespace

void WallClock::Init()
{
    app::Clusters::TimeSynchronization::SetDefaultDelegate(this);
    TEMPORARY_RETURN_IGNORED DeviceLayer::SystemLayer().StartTimer(kRetryInterval, RetryTimeSync, this);
}

// The cluster only asks the trusted time source at boot and when the source is set. At boot the
// Thread network is often not up yet, so the attempt fails and is never repeated. Until we have the
// time, repeat what the cluster does at boot.
void WallClock::RetryTimeSync(System::Layer * layer, void * context)
{
    if (static_cast<WallClock *>(context)->mSynced)
    {
        return;
    }

    auto * cluster = app::Clusters::TimeSynchronization::GetClusterInstance();
    if (cluster != nullptr)
    {
        DeviceLayer::ChipDeviceEvent event = {};
        event.Type                         = DeviceLayer::DeviceEventType::kServerReady;
        cluster->OnPlatformEventFn(event);
    }

    TEMPORARY_RETURN_IGNORED layer->StartTimer(kRetryInterval, RetryTimeSync, context);
}

void WallClock::UTCTimeAvailabilityChanged(uint64_t matterEpochMicros)
{
    mMatterEpochMicros     = matterEpochMicros;
    mMonotonicMicrosAtSync = System::SystemClock().GetMonotonicMicroseconds64().count();
    mSynced                = true;

    ChipLogProgress(AppServer, "Wall clock set: %lu s since the Matter epoch",
                    static_cast<unsigned long>(matterEpochMicros / kMicrosecondsPerSecond));
}

bool WallClock::GetMatterEpochSeconds(uint32_t & seconds) const
{
    VerifyOrReturnValue(mSynced, false);

    uint64_t elapsed = System::SystemClock().GetMonotonicMicroseconds64().count() - mMonotonicMicrosAtSync;
    seconds          = static_cast<uint32_t>((mMatterEpochMicros + elapsed) / kMicrosecondsPerSecond);
    return true;
}
