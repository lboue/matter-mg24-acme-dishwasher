#include "WallClock.h"

#include <app/clusters/time-synchronization-server/CodegenIntegration.h>
#include <lib/support/logging/CHIPLogging.h>
#include <system/SystemClock.h>

using namespace chip;

WallClock WallClock::sInstance;

void WallClock::Init()
{
    app::Clusters::TimeSynchronization::SetDefaultDelegate(this);
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
