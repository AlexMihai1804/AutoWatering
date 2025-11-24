/*
 * AutoWatering power management glue for nRF52 targets.
 * Keep hooks minimal until SoC-specific low-power sequencing is validated.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/pm.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_SOC_FAMILY_NRF)
#include <hal/nrf_power.h>
#endif

LOG_MODULE_REGISTER(autowatering_pm, CONFIG_LOG_DEFAULT_LEVEL);

void pm_state_set(enum pm_state state, uint8_t substate_id)
{
    ARG_UNUSED(substate_id);

    switch (state) {
#if defined(CONFIG_SOC_FAMILY_NRF)
    case PM_STATE_RUNTIME_IDLE:
    case PM_STATE_SUSPEND_TO_IDLE:
        /* Select System ON low power mode before yielding to WFI. */
        nrf_power_task_trigger(NRF_POWER, NRF_POWER_TASK_LOWPWR);
        k_cpu_idle();
        return;
    case PM_STATE_SOFT_OFF:
        /* Enter System OFF; execution does not return. */
        nrf_power_system_off(NRF_POWER);
        CODE_UNREACHABLE;
        break;
#else
    case PM_STATE_RUNTIME_IDLE:
    case PM_STATE_SUSPEND_TO_IDLE:
        k_cpu_idle();
        return;
#endif
    default:
        LOG_WRN("Unsupported PM state %d/%u, falling back to idle", state, substate_id);
        k_cpu_idle();
        break;
    }
}

void pm_state_exit_post_ops(enum pm_state state, uint8_t substate_id)
{
    ARG_UNUSED(state);
    ARG_UNUSED(substate_id);
}
