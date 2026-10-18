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

        int64_t cost    = 0;
        uint32_t stepAt = start;
        for (const auto & step : profile)
        {
            int64_t stepCost = 0;
            if (!Cost(periods, stepAt, stepAt + step.durationS, stepCost))
            {
                return;
            }
            cost += stepCost * step.power;
            stepAt += step.durationS;
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

} // namespace StartPlanner
