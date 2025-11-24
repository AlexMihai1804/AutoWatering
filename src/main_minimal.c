/**
 * @file main_minimal.c
 * @brief Minimal main for compilation test
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/usb/usb_device.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* LED */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
    LOG_INF("AutoWatering System Started - Minimal Version");
    
    /* Initialize USB */
    if (usb_enable(NULL)) {
        LOG_ERR("Failed to enable USB");
    }
    
    /* Initialize LED */
    if (gpio_is_ready_dt(&led)) {
        gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
    }
    
    /* Simple blink loop */
    while (1) {
        if (gpio_is_ready_dt(&led)) {
            gpio_pin_toggle_dt(&led);
        }
        k_sleep(K_MSEC(1000));
        LOG_INF("System running...");
    }
    
    return 0;
}
