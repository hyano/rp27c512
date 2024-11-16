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

#include "busmon.pio.h"

static uint8_t __noinit(busmon_wr_value_table[0x100]) __attribute__ ((aligned (0x100)));

static void busmon_wr_init(PIO pio, uint sm, uint offset, uint pin, uint8_t *ram)
{
    for (int32_t i = 0; i < 0x100; i++)
    {
        busmon_wr_value_table[i] = (uint8_t)i;
    }

    pio_sm_set_consecutive_pindirs(pio, sm, pin, busmon_wr_bit_width, false);
    pio_sm_config c = busmon_wr_program_get_default_config(offset);
    sm_config_set_in_pins(&c, pin);
    sm_config_set_in_shift(&c, true, false, 32);
    sm_config_set_clkdiv_int_frac(&c, 1, 0);
    pio_sm_init(pio, sm, offset + busmon_wr_offset_entry_point, &c);
    pio_sm_set_enabled(pio, sm, true);

    uint32_t ramaddr_top = (uint32_t)ram >> busmon_wr_addr_bus_width;
    pio_sm_put_blocking(pio, sm, ramaddr_top);
    uint32_t valaddr_top = (uint32_t)busmon_wr_value_table >> busmon_wr_data_bus_width;
    pio_sm_put_blocking(pio, sm, valaddr_top);
}


#define BUSMON_CAP_BUFFER_SHIFT (6)
#define BUSMON_CAP_BUFFER_COUNT (1 << BUSMON_CAP_BUFFER_SHIFT)
static uint32_t __noinit(busmon_cap_buffer[BUSMON_CAP_BUFFER_COUNT]) __attribute__ ((aligned (sizeof(uint32_t) * BUSMON_CAP_BUFFER_COUNT)));
static uint32_t busmon_cap_buffer_count = BUSMON_CAP_BUFFER_COUNT;
static int busmon_cap_dma_ch = 0;
static volatile uint32_t *busmon_cap_buffer_wp_ptr = NULL;
static uint32_t busmon_cap_buffer_rp = 0;

static void busmon_cap_init(PIO pio, uint sm, uint offset, uint pin)
{
    pio_sm_set_consecutive_pindirs(pio, sm, pin, busmon_cap_bit_width, false);
    pio_sm_config c = busmon_cap_program_get_default_config(offset);
    sm_config_set_in_pins(&c, pin);
    sm_config_set_in_shift(&c, true, false, 32);
    sm_config_set_clkdiv_int_frac(&c, 1, 0);
    pio_sm_init(pio, sm, offset + busmon_cap_offset_entry_point, &c);
    pio_sm_set_enabled(pio, sm, true);
}


void busmon_init(PIO pio, uint pin, uint8_t *ram)
{
    dma_channel_config c;

    // Capture write data
    uint offset_busmon_wr = pio_add_program(pio, &busmon_wr_program);
    uint sm_busmon_wr = pio_claim_unused_sm(pio, true);
    busmon_wr_init(pio, sm_busmon_wr, offset_busmon_wr, pin, ram);

    int dma_wr_addr = dma_claim_unused_channel(true);
    int dma_wr_data = dma_claim_unused_channel(true);

    c = dma_channel_get_default_config(dma_wr_addr);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, 3);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm_busmon_wr, false));
    dma_channel_configure(
        dma_wr_addr,
        &c,
        &dma_hw->ch[dma_wr_data].al2_read_addr,
        &pio->rxf[sm_busmon_wr],
        2,
        false
    );

    c = dma_channel_get_default_config(dma_wr_data);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_chain_to(&c, dma_wr_addr);
    dma_channel_configure(
        dma_wr_data,
        &c,
        NULL,
        NULL,
        1,
        false
    );

    dma_channel_start(dma_wr_addr);

    // Capture data access
    uint offset_busmon_cap = pio_add_program(pio, &busmon_cap_program);
    uint sm_busmon_cap = pio_claim_unused_sm(pio, true);
    busmon_cap_init(pio, sm_busmon_cap, offset_busmon_cap, pin);

    int dma_cap_trig = dma_claim_unused_channel(true);
    int dma_cap_buff = dma_claim_unused_channel(true);

    c = dma_channel_get_default_config(dma_cap_trig);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    dma_channel_configure(
        dma_cap_trig,
        &c,
        &dma_hw->ch[dma_cap_buff].al1_transfer_count_trig,
        &busmon_cap_buffer_count,
        1,
        false
    );

    c = dma_channel_get_default_config(dma_cap_buff);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, BUSMON_CAP_BUFFER_SHIFT + 2);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm_busmon_cap, false));
    channel_config_set_chain_to(&c, dma_cap_trig);
    dma_channel_configure(
        dma_cap_buff,
        &c,
        busmon_cap_buffer,
        &pio->rxf[sm_busmon_cap],
        BUSMON_CAP_BUFFER_COUNT,
        false
    );

    busmon_cap_dma_ch = dma_cap_buff;
    busmon_cap_buffer_wp_ptr = &dma_hw->ch[busmon_cap_dma_ch].write_addr;
    dma_channel_start(dma_cap_trig);
}

static inline uint32_t busmon_cap_buffer_wp(void)
{
    return (*busmon_cap_buffer_wp_ptr / 4) % BUSMON_CAP_BUFFER_COUNT;
}

void busmon_cap_start(void)
{
    busmon_cap_buffer_rp = busmon_cap_buffer_wp();
}

bool busmon_cap_is_empty(void)
{
    return (busmon_cap_buffer_rp == busmon_cap_buffer_wp());
}

uint32_t busmon_cap_pop(void)
{
    const uint32_t ret = busmon_cap_buffer[busmon_cap_buffer_rp];
    busmon_cap_buffer_rp = (busmon_cap_buffer_rp + 1) % BUSMON_CAP_BUFFER_COUNT;
    return ret;
}