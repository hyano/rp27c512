/*
 * Copyright (c) 2024 Hirokuni Yano
 *
 * Released under the MIT license.
 * see https://opensource.org/licenses/MIT
 */
#ifndef ROMEMU_H__
#define ROMEMU_H__

#include <stdint.h>
#include "hardware/pio.h"

void romemu_init(PIO pio, uint pin, uint8_t *rom);

#endif
