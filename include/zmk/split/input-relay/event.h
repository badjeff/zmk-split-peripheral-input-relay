/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

struct zmk_split_bt_input_event {
  uint8_t relay_channel;
	uint8_t sync;
	uint8_t type;
	uint16_t code;
	int32_t value;
} __packed;
