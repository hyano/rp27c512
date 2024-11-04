/*
 * Copyright (c) 2024 Hirokuni Yano
 *
 * Released under the MIT license.
 * see https://opensource.org/licenses/MIT
 */
#include <stdint.h>
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "section.h"

#include "romemu.pio.h"


static void romemu_io_init(PIO pio, uint sm, uint offset, uint pin, uint8_t *rom)
{
    const uint pin_addr = pin + romemu_io_addr_bus_offset;
    const uint pin_data = pin + romemu_io_data_bus_offset;
    for (uint i = 0; i < romemu_io_data_bus_width; i++)
    {
        pio_gpio_init(pio, pin_data + i);
    }
    pio_sm_set_consecutive_pindirs(pio, sm, pin_addr, romemu_io_addr_bus_width, false);
    pio_sm_set_consecutive_pindirs(pio, sm, pin_data, romemu_io_data_bus_width, false);
    pio_sm_config c = romemu_io_program_get_default_config(offset);
    sm_config_set_in_pins(&c, pin_addr);
    sm_config_set_in_shift(&c, true, true, 32);
    sm_config_set_out_pins(&c, pin_data, romemu_io_data_bus_width);
    sm_config_set_out_shift(&c, true, true, 8);
    sm_config_set_clkdiv_int_frac(&c, 1, 0);
    pio_sm_init(pio, sm, offset + romemu_io_offset_entry_point, &c);
    pio_sm_set_enabled(pio, sm, true);

    uint32_t romaddr_top = (uint32_t)rom >> romemu_io_addr_bus_width;
    pio_sm_put_blocking(pio, sm, romaddr_top);
}

static void romemu_oe_init(PIO pio, uint sm, uint offset, uint pin)
{
    const uint pin_ctrl = pin + romemu_oe_ctrl_bus_offset;
    const uint pin_data = pin + romemu_oe_data_bus_offset;
    pio_sm_set_consecutive_pindirs(pio, sm, pin_ctrl, romemu_oe_ctrl_bus_width, false);
    pio_sm_set_consecutive_pindirs(pio, sm, pin_data, romemu_oe_data_bus_width, false);
    pio_sm_config c = romemu_oe_program_get_default_config(offset);
    sm_config_set_in_pins(&c, pin_ctrl);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_out_pins(&c, pin_data, romemu_oe_data_bus_width);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_clkdiv_int_frac(&c, 1, 0);
    pio_sm_init(pio, sm, offset + romemu_oe_offset_entry_point, &c);
    pio_sm_set_enabled(pio, sm, true);
}


void romemu_init(PIO pio, uint pin, uint8_t *rom)
{
    dma_channel_config c;

    uint offset_romemu_io = pio_add_program(pio, &romemu_io_program);
    uint sm_romemu_io = pio_claim_unused_sm(pio, true);
    romemu_io_init(pio, sm_romemu_io, offset_romemu_io, pin, rom);

    uint offset_oe = pio_add_program(pio, &romemu_oe_program);
    uint sm_oe = pio_claim_unused_sm(pio, true);
    romemu_oe_init(pio, sm_oe, offset_oe, pin);


    int dma_addr = dma_claim_unused_channel(true);
    int dma_data = dma_claim_unused_channel(true);

    c = dma_channel_get_default_config(dma_addr);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm_romemu_io, false));
    dma_channel_configure(
        dma_addr,
        &c,
        &dma_hw->ch[dma_data].al3_read_addr_trig,
        &pio->rxf[sm_romemu_io],
        1,
        false
    );

    c = dma_channel_get_default_config(dma_data);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_chain_to(&c, dma_addr);
    dma_channel_configure(
        dma_data,
        &c,
        &pio->txf[sm_romemu_io],
        NULL,
        1,
        false
    );

    dma_channel_start(dma_addr);
}
