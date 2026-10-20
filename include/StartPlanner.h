#pragma once

#include <lib/support/Span.h>

#include <stdint.h>

/**
 * Picks the start time that makes a program cheapest (or greenest), from a list of periods each
 * with a value per unit of energy, such as a price or a carbon intensity, and the program's power
 * profile.
 */
namespace StartPlanner {

struct Period
{
    uint32_t start = 0; // Matter epoch seconds
    uint32_t end   = 0; // 0 when open-ended
    int64_t value  = 0;
};

// One step of the program, at constant power.
struct Step
{
    uint32_t durationS = 0;
    int64_t power      = 0;
};

/**
 * The total of value × power × seconds for running the profile from start. Where periods overlap,
 * the one that started most recently applies. Returns false if the periods don't cover the run.
 */
bool ProfileCost(chip::Span<const Period> periods, chip::Span<const Step> profile, uint32_t start, int64_t & cost);

/**
 * Tries starting now, and at every time that lines the start of a step up with the start or end of
 * a period, keeping only runs that end within horizonS of now and that the periods cover
 * completely. Where periods overlap, the one that started most recently applies. The lowest total
 * of value × power × seconds wins, and on a tie the earliest start.
 *
 * Returns false if no start qualifies.
 */
bool FindBestStart(chip::Span<const Period> periods, chip::Span<const Step> profile, uint32_t now, uint32_t horizonS,
                   uint32_t & delayS);

/**
 * Fills out with the significant times of day (00:00, 06:00, 09:00, 13:00, 18:00 and 21:00 UTC)
 * from earliest to latest inclusive, in order. Times are Matter epoch seconds, whose epoch starts
 * at midnight UTC. Returns how many it wrote, at most max.
 */
size_t SignificantTimes(uint32_t earliest, uint32_t latest, uint32_t * out, size_t max);

} // namespace StartPlanner
