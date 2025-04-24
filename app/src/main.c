/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/settings/settings.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_DISPLAY)

#include <zmk/display.h>
#include <lvgl.h>

#endif

static void set_static_mac_if_configured(void) {
#if defined(CONFIG_ZMK_CENTRAL_STATIC_MAC) && strlen(CONFIG_ZMK_CENTRAL_STATIC_MAC) > 0
    bt_addr_le_t addr;
    int err = bt_addr_le_from_str(CONFIG_ZMK_CENTRAL_STATIC_MAC, "public", &addr);
    if (err) {
        LOG_ERR("Failed to parse static MAC: %s (err %d)", CONFIG_ZMK_CENTRAL_STATIC_MAC, err);
        return;
    }

    err = bt_id_create(&addr, NULL);
    if (err < 0) {
        LOG_ERR("Failed to set static MAC address (err %d)", err);
    } else {
        LOG_INF("Central MAC address set to %s", CONFIG_ZMK_CENTRAL_STATIC_MAC);
    }
#endif
}

int main(void) {
    LOG_INF("Welcome to ZMK!\n");

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_subsys_init();
    settings_load();
#endif
    set_static_mac_if_configured();
#ifdef CONFIG_ZMK_DISPLAY
    zmk_display_init();
    set
#if IS_ENABLED(CONFIG_ARCH_POSIX)
        // Workaround for an SDL display issue:
        // https://github.com/zephyrproject-rtos/zephyr/issues/71410
        while (1) {
        lv_task_handler();
        k_sleep(K_MSEC(10));
    }
#endif

#endif /* CONFIG_ZMK_DISPLAY */

    return 0;
}
