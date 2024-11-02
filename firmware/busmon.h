/*
 * Copyright (c) 2024 Hirokuni Yano
 *
 * Released under the MIT license.
 * see https://opensource.org/licenses/MIT
 */
#ifndef BUSMON_H__
#define BUSMON_H__

#include <stdint.h>
#include "hardware/pio.h"

void busmon_init(PIO pio, uint pin, uint8_t *ram);
void busmon_cap_start(void);
bool busmon_cap_is_empty(void);
uint32_t busmon_cap_pop(void);

#endif
