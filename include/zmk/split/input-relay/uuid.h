/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/bluetooth/uuid.h>

/* 
  Define an alternative BT Split Service UUID   [last byte: 0x2b]
  Main BT Split Service UUID retained unchanged [last byte: 0x2a]
*/
#define ZMK_BT_SPLIT_ALT_UUID(num) BT_UUID_128_ENCODE(num, 0x0096, 0x7107, 0xc967, 0xc5cfb1c2482b)

#define ZMK_SPLIT_BT_ALT_SERVICE_UUID ZMK_BT_SPLIT_ALT_UUID(0x00000000)
#define ZMK_SPLIT_BT_ALT_CHAR_INPUT_STATE_UUID ZMK_BT_SPLIT_ALT_UUID(0x00000001)
