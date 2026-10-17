#pragma once

#include <lib/support/Span.h>

#include <stdint.h>

/**
 * Picks the start time that makes a program cheapest (or greenest), from a list of periods each
 * with a value per second, such as a price or a carbon intensity.
 */
namespace StartPlanner {

struct Period
{
    uint32_t start = 0; // Matter epoch seconds
    uint32_t end   = 0; // 0 when open-ended
    int64_t value  = 0;
};

/**
 * Tries starting now and at the start of every later period, keeping only runs that end within
 * horizonS of now and that the periods cover completely. Where periods overlap, the one that
 * started most recently applies. The lowest total of value × seconds wins, and on a tie the
 * earliest start.
 *
 * Returns false if no start qualifies.
 */
bool FindBestStart(chip::Span<const Period> periods, uint32_t now, uint32_t durationS, uint32_t horizonS, uint32_t & delayS);

} // namespace StartPlanner
