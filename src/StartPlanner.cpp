#include "StartPlanner.h"

#include <limits>

namespace StartPlanner {

namespace {
constexpr uint32_t kOpenEnded = std::numeric_limits<uint32_t>::max();

uint32_t EndOf(const Period & period)
{
    return period.end == 0 ? kOpenEnded : period.end;
}

// Adds up value × seconds over [start, end). False if part of it isn't covered by any period.
bool Cost(chip::Span<const Period> periods, uint32_t start, uint32_t end, int64_t & cost)
{
    cost       = 0;
    uint32_t t = start;
    while (t < end)
    {
        // The period covering t that started most recently, and the next start after t, which
        // cuts it short.
        const Period * current = nullptr;
        uint32_t nextStart     = kOpenEnded;
        for (const auto & period : periods)
        {
            if (period.start <= t && t < EndOf(period))
            {
                if (current == nullptr || period.start > current->start)
                {
                    current = &period;
                }
            }
            else if (period.start > t && period.start < nextStart)
            {
                nextStart = period.start;
            }
        }

        if (current == nullptr)
        {
            return false;
        }

        uint32_t segmentEnd = EndOf(*current);
        if (nextStart < segmentEnd)
        {
            segmentEnd = nextStart;
        }
        if (end < segmentEnd)
        {
            segmentEnd = end;
        }

        cost += current->value * static_cast<int64_t>(segmentEnd - t);
        t = segmentEnd;
    }
    return true;
}
} // namespace

bool ProfileCost(chip::Span<const Period> periods, chip::Span<const Step> profile, uint32_t start, int64_t & cost)
{
    cost            = 0;
    uint32_t stepAt = start;
    for (const auto & step : profile)
    {
        int64_t stepCost = 0;
        if (!Cost(periods, stepAt, stepAt + step.durationS, stepCost))
        {
            return false;
        }
        cost += stepCost * step.power;
        stepAt += step.durationS;
    }
    return true;
}

bool FindBestStart(chip::Span<const Period> periods, chip::Span<const Step> profile, uint32_t now, uint32_t horizonS,
                   uint32_t & delayS)
{
    uint32_t durationS = 0;
    for (const auto & step : profile)
    {
        durationS += step.durationS;
    }

    bool found        = false;
    uint32_t bestTime = 0;
    int64_t bestCost  = 0;

    auto consider = [&](uint32_t start) {
        if (start < now || start - now + durationS > horizonS)
        {
            return;
        }

        int64_t cost = 0;
        if (!ProfileCost(periods, profile, start, cost))
        {
            return;
        }

        if (!found || cost < bestCost || (cost == bestCost && start < bestTime))
        {
            found    = true;
            bestTime = start;
            bestCost = cost;
        }
    };

    // Lines each step up with each period boundary, so a heating step can land on a cheap period.
    auto considerBoundary = [&](uint32_t boundary) {
        uint32_t offset = 0;
        for (const auto & step : profile)
        {
            if (boundary >= offset)
            {
                consider(boundary - offset);
            }
            offset += step.durationS;
        }
    };

    consider(now);
    for (const auto & period : periods)
    {
        considerBoundary(period.start);
        if (period.end != 0)
        {
            considerBoundary(period.end);
        }
    }

    if (found)
    {
        delayS = bestTime - now;
    }
    return found;
}

size_t SignificantTimes(uint32_t earliest, uint32_t latest, uint32_t * out, size_t max)
{
    constexpr uint32_t kDayS     = 24 * 60 * 60;
    constexpr uint8_t kHours[]   = { 0, 6, 9, 13, 18, 21 };
    constexpr size_t kHoursCount = sizeof(kHours) / sizeof(kHours[0]);

    size_t count = 0;
    for (uint32_t day = earliest / kDayS * kDayS; day <= latest && count < max; day += kDayS)
    {
        for (size_t i = 0; i < kHoursCount && count < max; i++)
        {
            const uint32_t time = day + kHours[i] * 3600u;
            if (time >= earliest && time <= latest)
            {
                out[count++] = time;
            }
        }
    }
    return count;
}

} // namespace StartPlanner
