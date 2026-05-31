#pragma once

#include <cstdint>

namespace cdc::core {

/**
 * \brief On-demand aggregate CPU-load read-out from FreeRTOS run-time stats.
 *
 * Requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS. Load is derived from the
 * idle-task run-time counters versus wall-clock time across all cores. Nothing
 * is sampled in the background; a caller measures only when it asks.
 */
class CpuStats {
public:
    /**
     * \brief Snapshot cumulative idle CPU time and the wall-clock reference.
     * \param idleUs Out: summed run-time of every core's idle task, in run-time
     *        counter units (microseconds with the esp_timer source).
     * \param wallUs Out: current wall clock from esp_timer, in microseconds.
     * \return true on success, false if run-time stats are unavailable.
     */
    static bool sample(uint64_t& idleUs, uint64_t& wallUs);

    /**
     * \brief Measure aggregate CPU load over a blocking window.
     * \param windowMs Measurement window in milliseconds.
     * \return CPU load across all cores as 0..100 percent.
     */
    static uint8_t loadOverWindow(uint32_t windowMs = 250);
};

} // namespace cdc::core
