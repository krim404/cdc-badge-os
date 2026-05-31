#include "cdc_core/CpuStats.h"
#include "cdc_core/Raii.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include <cstring>

namespace cdc::core {

bool CpuStats::sample(uint64_t& idleUs, uint64_t& wallUs)
{
#if (configGENERATE_RUN_TIME_STATS == 1) && (configUSE_TRACE_FACILITY == 1)
    UBaseType_t count = uxTaskGetNumberOfTasks();
    if (count == 0) return false;

    auto buf = psramAlloc<TaskStatus_t>(count);
    if (!buf) return false;

    UBaseType_t got = uxTaskGetSystemState(buf.get(), count, nullptr);
    uint64_t idle = 0;
    for (UBaseType_t i = 0; i < got; ++i) {
        // ESP-IDF SMP names the per-core idle tasks "IDLE0"/"IDLE1"; sum them.
        if (buf[i].pcTaskName && std::strncmp(buf[i].pcTaskName, "IDLE", 4) == 0) {
            idle += buf[i].ulRunTimeCounter;
        }
    }

    idleUs = idle;
    wallUs = static_cast<uint64_t>(esp_timer_get_time());
    return true;
#else
    (void)idleUs;
    (void)wallUs;
    return false;
#endif
}

uint8_t CpuStats::loadOverWindow(uint32_t windowMs)
{
    uint64_t idle0, wall0, idle1, wall1;
    if (!sample(idle0, wall0)) return 0;
    vTaskDelay(pdMS_TO_TICKS(windowMs));
    if (!sample(idle1, wall1)) return 0;

    uint64_t wallDelta = (wall1 - wall0) * static_cast<uint64_t>(configNUMBER_OF_CORES);
    if (wallDelta == 0) return 0;
    uint64_t idleDelta = idle1 - idle0;
    if (idleDelta > wallDelta) idleDelta = wallDelta;  // clamp counter-wrap glitch
    return static_cast<uint8_t>(100 - (idleDelta * 100) / wallDelta);
}

} // namespace cdc::core
