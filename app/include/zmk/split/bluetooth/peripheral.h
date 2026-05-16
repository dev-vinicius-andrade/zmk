/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

bool zmk_split_bt_peripheral_is_connected(void);

bool zmk_split_bt_peripheral_is_bonded(void);

/**
 * Switch the active dongle slot.
 *
 * Slot 0 = first bonded dongle (typically home), slot 1 = second, etc.
 * The peripheral disconnects from the current dongle (if connected) and
 * starts advertising toward the newly selected slot.  If the slot has not
 * yet been paired, undirected advertising is used so a new bond can form.
 *
 * @param slot  0-based slot index (must be < CONFIG_ZMK_SPLIT_PERIPHERAL_DONGLE_PROFILES).
 * @return 0 on success, -EINVAL if the slot index is out of range.
 */
int zmk_split_bt_peripheral_select_dongle(uint8_t slot);