/*
 * Copyright (c) 2024 Hirokuni Yano
 *
 * Released under the MIT license.
 * see https://opensource.org/licenses/MIT
 */
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <tusb.h>

#include "microrl.h"
#include "xmodem.h"
#include "readline.h"
#include "section.h"

#include "busmon.h"
#include "romemu.h"

// #define DEBUG_PULL_UP

#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

//                /0123456789ABCDEF
#define MAGIC_STR "RP27C512 VER1.00"
#define MAGIC_SIZE (16)

#define GPIO_ADDR 0
#define GPIO_ADDR_END 16
#define GPIO_DATA 16 // 0-7
#define GPIO_DATA_END 24
#define GPIO_EXT0 24
#define GPIO_EXT1 25
#define GPIO_EXT2 26
#define GPIO_CE 27
#define GPIO_OE 28
#define GPIO_WR 29
#define GPIO_END 30

#define GPIO_ALL_MASK ((1 << GPIO_END) - 1)
#define GPIO_ADDR_MASK (((1 << (GPIO_ADDR_END - GPIO_ADDR)) - 1) << GPIO_ADDR)
#define GPIO_DATA_MASK (((1 << (GPIO_DATA_END - GPIO_DATA)) - 1) << GPIO_DATA)
#define GPIO_CE_MASK (1 << GPIO_CE)
#define GPIO_OE_MASK (1 << GPIO_OE)
#define GPIO_EXT0_MASK (1 << GPIO_EXT0)
#define GPIO_EXT1_MASK (1 << GPIO_EXT1)
#define GPIO_EXT2_MASK (1 << GPIO_EXT2)
#define GPIO_EXT_MASK (GPIO_EXT0_MASK | GPIO_EXT1_MASK | GPIO_EXT2_MASK)

#define GPIO_GET_DATA(v) (((v) & GPIO_DATA_MASK) >> GPIO_DATA)


#define CPU_CLOCK_FREQ_HIGH         (400 * 1000)
#define CPU_CLOCK_FREQ_NORMAL       (250 * 1000)
#define REBOOT_DELAY_MS             (250)
#define FLASH_WAIT_MS               (500)
#define DEFAULT_CLONE_WAIT_S        (5)
#define DEFAULT_CLONE_VERIFY_NUM    (2)
#define DEFAULT_DUMP_LINE_COUNT     (16)

#define CONFIG_ERASE_SIZE           (FLASH_SECTOR_SIZE * 3)
#define CONFIG_WRITE_SIZE           (FLASH_PAGE_SIZE * (1 + 32))

static uint8_t __memimage(rom[0x10000]) __attribute__((aligned(0x10000)));;
static uint8_t __memimage(ram[0x10000]) __attribute__((aligned(0x10000)));;
static uint8_t *device = rom;

static uint8_t __noinit(init_rom_data[FLASH_PAGE_SIZE]);

#define CAPTURE_COUNT 8192
static uint32_t __noinit(capture_buffer[CAPTURE_COUNT]);
static volatile uint32_t capture_wp = 0;
static uint32_t capture_rp = 0;
static uint8_t __noinit(capture_target[0x10000 / 8]);

#define CONFIG_BANK_BLOCK (31)
static const uint32_t FLASH_TARGET_OFFSET_CONFIG = (CONFIG_BANK_BLOCK * 0x10000);
static const uint8_t *flash_target_contents_config = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET_CONFIG);

#define ROM_BANK_NUM (4)
#define ROM_BANK_BLOCK (30)
static const uint32_t FLASH_TARGET_OFFSET_ROM[ROM_BANK_NUM] =
{
    (ROM_BANK_BLOCK - 0) * 0x10000,
    (ROM_BANK_BLOCK - 1) * 0x10000,
    (ROM_BANK_BLOCK - 2) * 0x10000,
    (ROM_BANK_BLOCK - 3) * 0x10000,
};
static const uint8_t *flash_target_contents_rom[ROM_BANK_NUM] =
{
    (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET_ROM[0]),
    (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET_ROM[1]),
    (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET_ROM[2]),
    (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET_ROM[3]),
};

typedef enum config_mode
{
    CONFIG_MODE_EMULATOR = 0,
    CONFIG_MODE_CLONE,
    CONFIG_MODE_NUM
} config_mode_e;

typedef struct
{
    uint32_t dir;
    uint32_t value;
    uint32_t pullup;
    uint32_t pulldown;
} gpio_config_t;

typedef struct
{
    char            magic[MAGIC_SIZE];
    config_mode_e   mode;
    int32_t         rom_bank;
    int32_t         dump_line_count;
    gpio_config_t   gpio_config;
    uint8_t         capture_target[0x10000 / 8];
} config_t;

typedef union
{
    config_t cfg;
    uint8_t bin[CONFIG_WRITE_SIZE];
} config_u;

config_u __noinit(config);
gpio_config_t __noinit(gpio_config);

static inline uint32_t bit(uint32_t bn)
{
    return (1 << bn);
}

static inline bool btst(uint32_t bm, uint32_t bn)
{
    return (bm & bit(bn)) != 0;
}


static void config_init(void)
{
    memset(&config, 0, sizeof(config));
    memcpy(config.cfg.magic, MAGIC_STR, MAGIC_SIZE);

    config.cfg.mode = CONFIG_MODE_EMULATOR;
    config.cfg.rom_bank = 0;
    config.cfg.dump_line_count = DEFAULT_DUMP_LINE_COUNT;

    config.cfg.gpio_config.dir = 0x00000000;
    config.cfg.gpio_config.pulldown = GPIO_EXT_MASK;
}

static bool config_is_valid(void)
{
    return memcmp(config.cfg.magic, MAGIC_STR, MAGIC_SIZE) == 0;
}

static bool config_load(void)
{
    int ch = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);

    dma_channel_configure(ch, &c, config.bin, flash_target_contents_config, sizeof(config) / 4, true);

    dma_channel_wait_for_finish_blocking(ch);

    dma_channel_unclaim(ch);

    return config_is_valid();
}

static bool config_save(void)
{
    uint32_t ints = save_and_disable_interrupts();
    multicore_lockout_start_blocking();
    flash_range_erase(FLASH_TARGET_OFFSET_CONFIG, CONFIG_ERASE_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET_CONFIG, config.bin, sizeof(config));
    multicore_lockout_end_blocking();
    restore_interrupts(ints);

    return memcmp(flash_target_contents_config, config.bin, sizeof(config)) == 0;
}

static bool config_save_init(void)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET_CONFIG, CONFIG_ERASE_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET_CONFIG, config.bin, sizeof(config));
    restore_interrupts(ints);

    return memcmp(flash_target_contents_config, config.bin, sizeof(config)) == 0;
}

static bool config_save_slow(void)
{
    bool ret;

    set_sys_clock_khz(CPU_CLOCK_FREQ_NORMAL, true);
    sleep_ms(FLASH_WAIT_MS);
    ret = config_save();
    sleep_ms(FLASH_WAIT_MS);
    set_sys_clock_khz(CPU_CLOCK_FREQ_HIGH, true);

    return ret;
}

static void ram_clear(void)
{
    static uint32_t zero = 0;
    int ch = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);

    dma_channel_configure(ch, &c, ram, &zero, sizeof(ram) / 4, true);

    dma_channel_wait_for_finish_blocking(ch);

    dma_channel_unclaim(ch);
}

static bool rom_load(int32_t bank)
{
    int ch = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);

    dma_channel_configure(ch, &c, rom, flash_target_contents_rom[bank], sizeof(rom) / 4, true);

    dma_channel_wait_for_finish_blocking(ch);

    dma_channel_unclaim(ch);

    return true;
}

static bool rom_load_slow(int32_t bank)
{
    bool ret;

    set_sys_clock_khz(CPU_CLOCK_FREQ_NORMAL, true);
    sleep_ms(FLASH_WAIT_MS);
    ret = rom_load(bank);
    sleep_ms(FLASH_WAIT_MS);
    set_sys_clock_khz(CPU_CLOCK_FREQ_HIGH, true);

    return ret;
}

static int32_t rom_load_async_start(int32_t bank)
{
    int ch = dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);

    dma_channel_configure(ch, &c, rom, flash_target_contents_rom[bank], sizeof(rom) / 4, true);

    return ch;
}

static bool rom_load_async_wait(int32_t ch)
{
    dma_channel_wait_for_finish_blocking(ch);

    dma_channel_unclaim(ch);

    return true;
}

static bool rom_save(int32_t bank)
{
    uint32_t ints = save_and_disable_interrupts();
    multicore_lockout_start_blocking();
    flash_range_erase(FLASH_TARGET_OFFSET_ROM[bank], sizeof(rom));
    flash_range_program(FLASH_TARGET_OFFSET_ROM[bank], rom, sizeof(rom));
    multicore_lockout_end_blocking();
    restore_interrupts(ints);

    return memcmp(flash_target_contents_rom[bank], rom, sizeof(rom)) == 0;
}

static bool rom_save_slow(int32_t bank)
{
    bool ret;

    set_sys_clock_khz(CPU_CLOCK_FREQ_NORMAL, true);
    sleep_ms(FLASH_WAIT_MS);
    ret = rom_save(bank);
    sleep_ms(FLASH_WAIT_MS);
    set_sys_clock_khz(CPU_CLOCK_FREQ_HIGH, true);

    return ret;
}

static bool rom_erase(int32_t bank)
{
    bool ret = true;
    memset(init_rom_data, 0xff, sizeof(init_rom_data));

    uint32_t ints = save_and_disable_interrupts();
    multicore_lockout_start_blocking();
    for (int32_t s = 0; s < sizeof(rom) / FLASH_SECTOR_SIZE; s++)
    {
        flash_range_erase(FLASH_TARGET_OFFSET_ROM[bank] + FLASH_SECTOR_SIZE * s, sizeof(rom));
        for (int32_t p = 0; p < FLASH_SECTOR_SIZE / sizeof(init_rom_data); p++)
        {
            flash_range_program(
                FLASH_TARGET_OFFSET_ROM[bank] + FLASH_SECTOR_SIZE * s + sizeof(init_rom_data) * p,
                init_rom_data,
                sizeof(init_rom_data));
            if (memcmp(
                flash_target_contents_rom[bank] + FLASH_SECTOR_SIZE * s + sizeof(init_rom_data) * p,
                init_rom_data,
                sizeof(init_rom_data)) != 0)
            {
                ret = false;
            }
        }
    }
    multicore_lockout_end_blocking();
    restore_interrupts(ints);

    return ret;
}

static bool rom_erase_slow(int32_t bank)
{
    bool ret;

    set_sys_clock_khz(CPU_CLOCK_FREQ_NORMAL, true);
    sleep_ms(FLASH_WAIT_MS);
    ret = rom_erase(bank);
    sleep_ms(FLASH_WAIT_MS);
    set_sys_clock_khz(CPU_CLOCK_FREQ_HIGH, true);

    return ret;
}


static void capture_target_enable(uint32_t start, uint32_t end)
{
    for (uint32_t addr = start; addr <= end; addr++)
    {
        capture_target[addr / 8] |= (1 << (addr % 8));
    }
}

static void capture_target_disable(uint32_t start, uint32_t end)
{
    for (uint32_t addr = start; addr <= end; addr++)
    {
        capture_target[addr / 8] &= ~(1 << (addr % 8));
    }
}

static inline bool capture_is_target(uint32_t addr)
{
    return (capture_target[addr / 8] & (1 << (addr % 8))) != 0;
}

static void reboot(uint32_t delay_ms)
{
    printf("rebooting...\n\n");
    fflush(stdout);

    watchdog_reboot(0, 0, delay_ms);

    for (;;)
    {
        tight_loop_contents();
    }
}

static void read_rom(uint8_t *dst, uint32_t start, uint32_t end)
{
    gpio_put_all(bit(GPIO_CE) | bit(GPIO_OE));
    sleep_us(1);
    for (uint32_t addr = start; addr < end; addr++)
    {
        gpio_put_all(addr | bit(GPIO_OE));
        sleep_us(1);
        gpio_put_all(addr);
        sleep_us(1);
        dst[addr] = GPIO_GET_DATA(gpio_get_all());
    }
    gpio_put_all(bit(GPIO_CE) | bit(GPIO_OE));
}

static void core1_entry_emulator(void)
{
    uint32_t cap;
    uint32_t rw = 3;
    uint32_t rw_prev;
    uint32_t addr;

    multicore_lockout_victim_init();

    busmon_cap_start();
    while (true)
    {
        if (!busmon_cap_is_empty())
        {
            cap = busmon_cap_pop();
            addr = cap & 0x00ffff;
            if (capture_is_target(addr))
            {
                capture_buffer[capture_wp] = cap;
                capture_wp = (capture_wp + 1) % CAPTURE_COUNT;
            }
        }
        tight_loop_contents();
    }
}

static void core1_entry_clone(void)
{
    multicore_lockout_victim_init();

    while (true)
    {
        sleep_ms(1000);
        tight_loop_contents();
    }
}

static void memdump(const uint8_t *mem, uint32_t addr, int32_t count)
{
    for (int y = 0; y < count; y++)
    {
        printf("%04x ", addr & 0xffff);
        for (int x = 0; x < 16; x++)
        {
            printf(" %02x", mem[(addr + x) & 0xffff]);
        }
        printf("  ");
        for (int x = 0; x < 16; x++)
        {
            int c = mem[(addr + x) & 0xffff];
            printf("%c", isprint(c) ? c : '.');
        }
        printf("\n");
        addr += 16;
    }
}

static void dump_diff(const uint8_t *mem0, const uint8_t *mem1, int32_t count, int32_t max_diff_count)
{
    int32_t diff_count = 0;
    for (int32_t idx = 0; idx < count; idx++)
    {
        if (mem0[idx] != mem1[idx])
        {
            diff_count++;
            if (diff_count <= max_diff_count)
            {
                printf("  %04x: %02x %02x\n", idx, mem0[idx], mem1[idx]);
            }
        }
    }
    printf(" %d missmatch(es)\n", diff_count);
}

static void cmd_hello(int argc, const char *const *argv)
{
    printf("hello world\n");
    for (int i = 0; i < argc; i++)
    {
        printf("%d: %s\n", i, argv[i]);
    }
}

static void cmd_reboot(int argc, const char *const *argv)
{
    uint32_t delay_ms = REBOOT_DELAY_MS;
    if (argc > 0)
    {
        delay_ms = strtol(argv[1], NULL, 10);
    }
    reboot(delay_ms);
}

static void cmd_mode(int argc, const char *const *argv)
{
    if (argc == 2)
    {
        if (strcmp(argv[1], "emulator") == 0)
        {
            config.cfg.mode = CONFIG_MODE_EMULATOR;
            printf("mode: emulator\n");
            config_save_slow();
            reboot(REBOOT_DELAY_MS);
            return;
        }
        else if (strcmp(argv[1], "clone") == 0)
        {
            config.cfg.mode = CONFIG_MODE_CLONE;
            printf("mode: clone\n");
            config_save_slow();
            reboot(REBOOT_DELAY_MS);
            return;
        }
        else
        {
            printf("error: unknown mode.\n");
        }
    }
    printf("mode emulator|clone\n");
    switch (config.cfg.mode)
    {
    case CONFIG_MODE_EMULATOR:
        printf("current mode: emulator\n");
        break;
    case CONFIG_MODE_CLONE:
        printf("current mode: clone\n");
        break;
    default:
        printf("current mode: unknown (%d)\n", config.cfg.mode);
        break;
    }
}

static void cmd_bootsel(int argc, const char *const *argv)
{
    if (argc > 0)
    {
        uint32_t delay_ms = REBOOT_DELAY_MS;
        delay_ms = strtol(argv[1], NULL, 10);
        sleep_ms(delay_ms);
    }
    reset_usb_boot(0, 0);
}

static struct
{
    const char* name;
    uint32_t pin;
} gpio_pin_name_table[] =
{
    {"a0",          GPIO_ADDR + 0},
    {"a1",          GPIO_ADDR + 1},
    {"a2",          GPIO_ADDR + 2},
    {"a3",          GPIO_ADDR + 3},
    {"a4",          GPIO_ADDR + 4},
    {"a5",          GPIO_ADDR + 5},
    {"a6",          GPIO_ADDR + 6},
    {"a7",          GPIO_ADDR + 7},
    {"a8",          GPIO_ADDR + 8},
    {"a9",          GPIO_ADDR + 9},
    {"a10",         GPIO_ADDR + 10},
    {"a11",         GPIO_ADDR + 11},
    {"a12",         GPIO_ADDR + 12},
    {"a13",         GPIO_ADDR + 13},
    {"a14",         GPIO_ADDR + 14},
    {"a15",         GPIO_ADDR + 15},
    {"d0",          GPIO_DATA + 0},
    {"d1",          GPIO_DATA + 1},
    {"d2",          GPIO_DATA + 2},
    {"d3",          GPIO_DATA + 3},
    {"d4",          GPIO_DATA + 4},
    {"d5",          GPIO_DATA + 5},
    {"d6",          GPIO_DATA + 6},
    {"d7",          GPIO_DATA + 7},
    {"ce",          GPIO_CE},
    {"oe",          GPIO_OE},
    {"wr",          GPIO_WR},
    {"ext0",        GPIO_EXT0},
    {"ext1",        GPIO_EXT1},
    {"ext2",        GPIO_EXT2},
    {NULL, 0}
};

static uint32_t get_gpio_pin(const char *s)
{
    for (int32_t idx = 0; gpio_pin_name_table[idx].name != NULL; idx++)
    {
        if (strcmp(s, gpio_pin_name_table[idx].name) == 0)
        {
            return gpio_pin_name_table[idx].pin;
        }
    }
    {
        uint32_t pin;
        char *end;
        pin = strtol(s, &end, 10);
        if (*end == '\0')
        {
            return pin;
        }
    }
    return UINT32_MAX;
}

static void cmd_gpio(int argc, const char *const *argv)
{
    if (argc > 1)
    {
        uint32_t pin = UINT32_MAX;
        if (argc == 3 && strcmp(argv[1], "in") == 0)
        {
            pin = get_gpio_pin(argv[2]);
            if ((1 << pin) & GPIO_EXT_MASK)
            {
                gpio_config.dir &= ~(1 << pin) & GPIO_EXT_MASK;
                gpio_set_dir_in_masked(~gpio_config.dir & GPIO_EXT_MASK);
                return;
            }
        }
        if (argc == 3 && strcmp(argv[1], "out") == 0)
        {
            pin = get_gpio_pin(argv[2]);
            if ((1 << pin) & GPIO_EXT_MASK)
            {
                gpio_config.dir |= (1 << pin) & GPIO_EXT_MASK;
                gpio_set_dir_out_masked(gpio_config.dir & GPIO_EXT_MASK);
                return;
            }
        }
        if (argc == 3 && strcmp(argv[1], "pullup") == 0)
        {
            pin = get_gpio_pin(argv[2]);
            if (pin < GPIO_END)
            {
                gpio_config.pullup |= (1 << pin) & GPIO_ALL_MASK;
                gpio_config.pulldown &= ~(1 << pin) & GPIO_ALL_MASK;
                gpio_pull_up(pin);
                return;
            }
        }
        if (argc == 3 && strcmp(argv[1], "pulldown") == 0)
        {
            pin = get_gpio_pin(argv[2]);
            if (pin < GPIO_END)
            {
                gpio_config.pullup &= ~(1 << pin) & GPIO_ALL_MASK;
                gpio_config.pulldown |= (1 << pin) & GPIO_ALL_MASK;
                gpio_pull_down(pin);
                return;
            }
        }
        if (argc == 3 && strcmp(argv[1], "pullno") == 0)
        {
            pin = get_gpio_pin(argv[2]);
            if (pin < GPIO_END)
            {
                gpio_config.pullup &= ~(1 << pin) & GPIO_ALL_MASK;
                gpio_config.pulldown &= ~(1 << pin) & GPIO_ALL_MASK;
                gpio_disable_pulls(pin);
                return;
            }
        }
        if (argc == 3 && strcmp(argv[1], "set") == 0)
        {
            pin = get_gpio_pin(argv[2]);
            if ((1 << pin) & GPIO_EXT_MASK)
            {
                gpio_config.value |= (1 << pin) & GPIO_EXT_MASK;
                gpio_put(pin, true);
                return;
            }
        }
        if (argc == 3 && strcmp(argv[1], "clr") == 0)
        {
            pin = get_gpio_pin(argv[2]);
            if ((1 << pin) & GPIO_EXT_MASK)
            {
                gpio_config.value &= ~(1 << pin) & GPIO_EXT_MASK;
                gpio_put(pin, false);
                return;
            }
        }
        if (argc > 2 && strcmp(argv[1], "pulse") == 0)
        {
            uint32_t width_us = 100;
            pin = get_gpio_pin(argv[2]);
            if (argc > 3)
            {
                width_us = strtol(argv[3], NULL, 10);
            }
            if ((1 << pin) & GPIO_EXT_MASK)
            {
                bool value = (gpio_config.value & (1 << pin)) != 0;
                gpio_put(pin, !value);
                sleep_us(width_us);
                gpio_put(pin, value);
                return;
            }
        }
        if (argc == 2 && strcmp(argv[1], "save") == 0)
        {
            printf("save GPIO settings ... ");
            config.cfg.gpio_config = gpio_config;
            config_save_slow();
            printf("done.\n");
            return;
        }
        {
            printf("gpio commands:\n");
            printf("  gpio in pin (only for ext0-2)\n");
            printf("  gpio out pin (only for ext0-2)\n");
            printf("  gpio pullup pin\n");
            printf("  gpio pulldown pin\n");
            printf("  gpio pullno pin\n");
            printf("  gpio set pin (only for ext0-2)\n");
            printf("  gpio clr pin (only for ext0-2)\n");
            printf("  gpio pulse pin width\n");
            printf("  gpio save\n");
            printf("pin name & pin no:\n");
            for (int32_t idx = 0; gpio_pin_name_table[idx].name != NULL; idx++)
            {
                printf("  %-4s %d\n", gpio_pin_name_table[idx].name, gpio_pin_name_table[idx].pin);
            }
            return;
        }
    }
    printf("GPIO:\n");
    printf(" current : %08x\n", gpio_get_all());
    printf(" dir     : %08x\n", gpio_config.dir);
    printf(" initial : %08x\n", gpio_config.value);
    printf(" pullup  : %08x\n", gpio_config.pullup);
    printf(" pulldown: %08x\n", gpio_config.pulldown);
}

static void cmd_device(int argc, const char *const *argv)
{
    if (argc > 1)
    {
        if (strcmp(argv[1], "ram") == 0)
        {
            device = ram;
        }
        else if (strcmp(argv[1], "rom") == 0)
        {
            device = rom;
        }
        else
        {
            printf("error: unknown device. only support ram or rom\n");
            return;
        }
    }
    else
    {
        printf("device ram|rom\n");
    }

    {
        const char *name = "unknown";
        if (device == ram)
        {
            name = "ram";
        }
        else if (device == rom)
        {
            name = "rom";
        }
        printf("current device: %s\n", name);
    }
}

static void cmd_dump(int argc, const char *const *argv)
{
    static uint32_t addr = 0;
    if (argc > 1)
    {
        addr = strtol(argv[1], NULL, 16);
    }
    memdump(device, addr, config.cfg.dump_line_count);
    addr += 0x100;
}

static void cmd_dump_watch(int argc, const char *const *argv)
{
    static uint32_t addr = 0;
    printf("\x1b[2J");
    printf("\x1b[?25l"); // cursor off
    if (argc > 1)
    {
        addr = strtol(argv[1], NULL, 16);
    }
    while (getchar_timeout_us(0) == PICO_ERROR_TIMEOUT)
    {
        printf("\x1b[0;0H");
        memdump(device, addr, config.cfg.dump_line_count);
    }
    addr += 0x100;
    printf("\x1b[?25h"); // cursor on
}

static void cmd_dump_len(int argc, const char *const *argv)
{
    static int32_t len = 0;
    if (argc > 1)
    {
        len = strtol(argv[1], NULL, 10);
        if (len > 0)
        {
            if (argc > 2)
            {
                if (strcmp(argv[2], "save") == 0)
                {
                    config.cfg.dump_line_count = len;
                    config_save_slow();
                }
                else
                {
                    printf("error: illegal save option\n");
                }
            }
            else
            {
                config.cfg.dump_line_count = len;
            }
        }
        else
        {
            printf("error: illegal dump line count\n");
        }
    }
    else
    {
        printf("dlen len [save]\n");
    }
    printf("current dump line count: %d\n", config.cfg.dump_line_count);
}

static void cmd_edit(int argc, const char *const *argv)
{
    static uint32_t addr = 0;
    char buffer[3];
    if (argc > 1)
    {
        addr = strtol(argv[1], NULL, 16) & 0xffff;
    }
    if (argc > 2)
    {
        uint32_t value;
        char *end;
        value = strtol(buffer, &end, 16);
        if (*end == '\0')
        {
            device[addr] = value;
        }
    }
    else
    {
        printf("edit end with '.'\n");
        for (;;)
        {
            printf("%04x %02x : ", addr, device[addr]);
            readline(buffer, sizeof(buffer));
            if (buffer[0] == '.')
            {
                break;
            }
            else if (buffer[0] != '\0')
            {
                uint32_t value;
                char *end;
                value = strtol(buffer, &end, 16);
                if (*end == '\0')
                {
                    device[addr] = value;
                }
            }

            addr = (addr + 1) & 0xffff;
        }
    }
}

static void cmd_move(int argc, const char *const *argv)
{
    if (argc > 3)
    {
        uint32_t addr;
        uint32_t start;
        uint32_t end;
        uint32_t dest;
        start = strtol(argv[1], NULL, 16) & 0xffff;
        end = strtol(argv[2], NULL, 16) & 0xffff;
        dest = strtol(argv[3], NULL, 16) & 0xffff;
        if (start <= end)
        {
            if (dest > start)
            {
                addr = end;
                dest = (dest + (end - start)) & 0xffff;
                for (;;)
                {
                    device[dest] = device[addr];
                    if  (addr == start) break;
                    dest = (dest - 1) & 0xffff;
                    addr = (addr - 1) & 0xffff;
                }
                return;
            }
            else if (dest < start)
            {
                addr = start;
                for (;;)
                {
                    device[dest] = device[addr];
                    if  (addr == end) break;
                    dest = (dest + 1) & 0xffff;
                    addr = (addr + 1) & 0xffff;
                }
                return;
            }
        }
    }
    printf("m start end dest\n");
}

static void cmd_fill(int argc, const char *const *argv)
{
    if (argc > 3)
    {
        uint32_t addr;
        uint32_t start;
        uint32_t end;
        uint32_t value;
        start = strtol(argv[1], NULL, 16) & 0xffff;
        end = strtol(argv[2], NULL, 16) & 0xffff;
        value = strtol(argv[3], NULL, 16) & 0xff;
        if (start <= end)
        {
            for (uint32_t addr = start; addr < end; addr++)
            {
                device[addr] = value;
            }
            return;
        }
    }
    printf("f start end value\n");
}

static void cmd_watch(int argc, const char *const *argv)
{
    if (argc > 2)
    {
        uint32_t start, end;
        start = strtol(argv[1], NULL, 16) & 0xffff;
        end = strtol(argv[2], NULL, 16) & 0xffff;
        printf("set capture area %04x %04x\n", start, end);
        capture_target_enable(start, end);
    }
    else
    {
        printf("watch start end\n");
    }
}

static void cmd_unwatch(int argc, const char *const *argv)
{
    if (argc > 2)
    {
        uint32_t start, end;
        start = strtol(argv[1], NULL, 16) & 0xffff;
        end = strtol(argv[2], NULL, 16) & 0xffff;
        printf("unset capture area %04x %04x\n", start, end);
        capture_target_disable(start, end);
    }
    else
    {
        printf("unwatch start end\n");
    }
}

static void cmd_capture(int argc, const char *const *argv)
{
    uint32_t cap;
    uint32_t addr;
    uint32_t data;
    uint32_t rw;
    static const char *str_rw = "-WRX";

    capture_rp = capture_wp;
    while (getchar_timeout_us(0) == PICO_ERROR_TIMEOUT)
    {
        if (capture_rp != capture_wp)
        {
            cap = capture_buffer[capture_rp];
            addr = cap & 0xffff;
            data = (cap >> 16) & 0xff;
            rw = cap >> (16 + 8 + 3 + 1);
            capture_rp = (capture_rp + 1) % CAPTURE_COUNT;

            printf("%c:%04x:%02x\n", str_rw[rw], addr, data);
        }
    }
}

static void cmd_list_watch(int argc, const char *const *argv)
{
    uint32_t start = 0x0000;
    uint32_t end = 0xffff;
    uint32_t addr;
    bool watch = false;
    const char *format = "%04x %04x\n";

    if (argc > 1)
    {
        start = strtol(argv[1], NULL, 16) & 0xffff;
    }
    if (argc > 2)
    {
        end = strtol(argv[2], NULL, 16) & 0xffff;
    }

    for (addr = start; addr <= end; addr++)
    {
        if (!watch)
        {
            if (capture_is_target(addr))
            {
                watch = true;
                start = addr;
            }
        }
        else
        {
            if (!capture_is_target(addr))
            {
                watch = false;
                printf(format, start, addr - 1);
            }
        }
    }
    if (watch)
    {
        printf(format, start, addr - 1);
    }
}

static void cmd_save_watch(int argc, const char *const *argv)
{
    printf("save capture area ... ");
    memcpy(config.cfg.capture_target, capture_target, sizeof(capture_target));
    config_save_slow();
    printf("done.\n");
}

int _inbyte(unsigned short timeout)
{
    return getchar_timeout_us((uint32_t)timeout * 1000);
}

void _outbyte(int c)
{
    putchar_raw(c);
}

static void cmd_recv(int argc, const char *const *argv)
{
    printf("receive data from host to device (XMODEM CRC)\n");
    XmodemReceiveCrc(NULL, device, sizeof(rom));
    sleep_ms(1000);
    printf("done.\n");
}

static void cmd_send(int argc, const char *const *argv)
{
    printf("send data from device to host (XMODEM 1K)\n");
    XmodemTransmit1K(NULL, device, sizeof(rom));
    sleep_ms(1000);
    printf("done.\n");
}

static void cmd_bank(int argc, const char *const *argv)
{
    if (argc > 1)
    {
        int32_t bank = -1;
        char *end;
        bank = strtol(argv[1], &end, 10);
        if ((*end == '\0') && (bank >= 0) && (bank <= 3))
        {
            config.cfg.rom_bank = bank;
            config_save_slow();

            printf("current rom bank: %d\n", config.cfg.rom_bank);
            return;
        }
        else
        {
            printf("error: illegal bank num\n");
        }
    }

    printf("bank 0|1|2|3\n");
    printf("current rom bank: %d\n", config.cfg.rom_bank);
}

static void cmd_load(int argc, const char *const *argv)
{
    bool ret;

    int32_t bank = config.cfg.rom_bank;

    printf("load rom bank %d ... ", bank);
    ret = rom_load_slow(bank);
    printf("done.\n");

    if (ret)
    {
        printf("load: OK\n");
    }
    else
    {
        printf("load: NG\n");
    }
}

static void cmd_save(int argc, const char *const *argv)
{
    bool ret;
    int32_t bank = config.cfg.rom_bank;

    printf("save rom bank %d ... ", bank);
    ret = rom_save_slow(bank);
    printf("done.\n");

    if (ret)
    {
        printf("save: OK\n");
    }
    else
    {
        printf("save: NG\n");
    }
}

static bool erase_rom_bank(int32_t bank)
{
    bool ret;

    printf("erase rom bank %d ... ", bank);
    ret = rom_erase_slow(bank);
    printf("done.\n");

    return ret;
}

static void cmd_erase(int argc, const char *const *argv)
{
    bool ret;
    int32_t bank;

    if (argc > 1)
    {
        char *end;
        bank = strtol(argv[1], &end, 10);
        if ((*end != '\0') || !((bank >= 0) && (bank <= 3)))
        {
            printf("error: illegal bank num\n");
            return;
        }
    }
    else
    {
        // for safety
        //bank = config.cfg.rom_bank;
        printf("erase 0,1,2,3\n");
        return;
    }

    ret = erase_rom_bank(bank);

    if (ret)
    {
        printf("erase: OK\n");
    }
    else
    {
        printf("erase: NG\n");
    }
}

static void cmd_clone(int argc, const char *const *argv)
{
    bool clone_ok = true;
    uint32_t wait_s = DEFAULT_CLONE_WAIT_S;
    int32_t verify_num = DEFAULT_CLONE_VERIFY_NUM;

    if (argc > 1)
    {
        wait_s = strtol(argv[1], NULL, 10);
    }
    if (argc > 2)
    {
        verify_num = strtol(argv[2], NULL, 10);
    }

    printf("read ROM\n");
    printf(" wait  : %d s\n", wait_s);
    printf(" verify: %d times\n", verify_num);

    sleep_ms(wait_s * 1000);

    printf("read start ... ");
    read_rom(rom, 0x0000, 0x10000);
    printf("done.\n");

    for (int32_t i = 0; i < verify_num; i++)
    {
        printf("verify (%d/%d) ... ", i + 1, verify_num);
        read_rom(ram, 0x0000, 0x10000);
        if (memcmp(rom, ram, sizeof(rom)) == 0)
        {
            printf("OK\n");
        }
        else
        {
            printf("NG\n");
            dump_diff(rom, ram, 0x10000, 16);
            clone_ok = false;
        }
    }
    printf("clone: %s\n", clone_ok ? "OK" : "NG");
}

static void cmd_init(int argc, const char *const *argv)
{
    bool init_config = false;
    bool init_rom = false;

    if (argc > 1)
    {
        do
        {
            if (strcmp(argv[1], "all") == 0)
            {
                init_config = true;
                init_rom = true;
            }
            else if (strcmp(argv[1], "rom") == 0)
            {
                init_rom = true;
            }
            else if (strcmp(argv[1], "config") == 0)
            {
                init_config = true;
            }
            else
            {
                printf("error: illegal parameter\n");
                break;
            }

            if (init_rom)
            {
                printf("erase all flash rom banks\n");
                for (int32_t bank = 0; bank < ROM_BANK_NUM; bank++)
                {
                    erase_rom_bank(bank);
                }
            }
            if (init_config)
            {
                printf("initialize configuration\n");
                config_init();
                config_save();

                reboot(REBOOT_DELAY_MS);
            }
            return;
        }
        while (false);
    }

    printf("init all|rom|config\n");
}

static void cmd_help(int argc, const char *const *argv);

typedef const struct
{
    char *name;
    void (*callback)(int argc, const char *const *argv);
    const char* help;
} command_table_t;

static const command_table_t command_table_emulator[] =
{
    {"help",    cmd_help,       "show help"},
    {"?",       cmd_help,       "show help"},

    {"hello",   cmd_hello,      "test: hello, world"},

    {"reboot",  cmd_reboot,     "reboot RP27C512"},
    {"mode",    cmd_mode,       "select mode (mode emulator|clone)"},
    {"bootsel", cmd_bootsel,    "reboot RP27C512 in BOOTSEL mode"},
    {"gpio",    cmd_gpio,       "show GPIO status"},

    {"device",  cmd_device,     "select device (device rom|ram)"},
    {"d",       cmd_dump,       "dump device (d address)"},
    {"dw",      cmd_dump_watch, "dump device repeatly (dw address)"},
    {"dlen",    cmd_dump_len,   "set dump line count (dlen count)"},

    {"e",       cmd_edit,       "edit memory"},
    {"m",       cmd_move,       "move memory"},
    {"f",       cmd_fill,       "fill memory"},

    {"watch",   cmd_watch,      "set capture area"},
    {"unwatch", cmd_unwatch,    "unset capture area"},
    {"cap",     cmd_capture,    "show capture log"},
    {"wlist",   cmd_list_watch, "list capture area"},
    {"wsave",   cmd_save_watch, "save capture area"},

    {"recv",    cmd_recv,       "receive data from host (XMODEM CRC)"},
    {"send",    cmd_send,       "send data to host (XMODEM 1K)"},

    {"bank",    cmd_bank,       "select flash rom bank (bank 0,1,2,3)"},
    {"load",    cmd_load,       "load data from current flash rom bank"},
    {"save",    cmd_save,       "save data to current flash rom bank"},
    {"erase",   cmd_erase,      "erase flash rom bank"},

    {"init",    cmd_init,       "initialize rom/config (init all|rom|config)"},

    {NULL, NULL}
};

static const command_table_t command_table_clone[] =
{
    {"help",    cmd_help,       "show help"},
    {"?",       cmd_help,       "show help"},

    {"hello",   cmd_hello,      "test: hello, world"},

    {"reboot",  cmd_reboot,     "reboot RP27C512"},
    {"mode",    cmd_mode,       "select mode (mode emulator|clone)"},
    {"bootsel", cmd_bootsel,    "reboot RP27C512 in BOOTSEL mode"},
    {"gpio",    cmd_gpio,       "show GPIO status"},

    {"d",       cmd_dump,       "dump device (d address)"},
    {"dlen",    cmd_dump_len,   "set dump line count (dlen count)"},

    {"e",       cmd_edit,       "edit memory"},
    {"m",       cmd_move,       "move memory"},
    {"f",       cmd_fill,       "fill memory"},

    {"recv",    cmd_recv,       "receive data from host (XMODEM CRC)"},
    {"send",    cmd_send,       "send data to host (XMODEM 1K)"},

    {"bank",    cmd_bank,       "select flash rom bank (bank 0,1,2,3)"},
    {"load",    cmd_load,       "load data from current flash rom bank"},
    {"save",    cmd_save,       "save data to current flash rom bank"},
    {"erase",   cmd_erase,      "erase flash rom bank"},

    {"clone",   cmd_clone,      "clone from real ROM chip (clone wait verify_num)"},

    {"init",    cmd_init,       "initialize rom/config (init all|rom|config)"},

    {NULL, NULL}
};

static char *complete_table[ARRAY_SIZE(command_table_emulator)];
static const command_table_t *command_table = command_table_emulator;

void cmd_help(int argc, const char *const *argv)
{
    for (int32_t i = 0; command_table[i].name != NULL; i++)
    {
        printf("%-8s: %s\n", command_table[i].name, command_table[i].help);
    }
}


static int mrl_print(microrl_t *mrl, const char *str)
{
    return printf("%s", str);
}

static int mrl_execute(microrl_t *mrl, int argc, const char *const *argv)
{
    for (int32_t i = 0; command_table[i].name != NULL; i++)
    {
        const char *name = command_table[i].name;
        if (strcmp(argv[0], name) == 0)
        {
            command_table[i].callback(argc, argv);
            return 0;
        }
    }
    printf("command not found: %s\n", argv[0]);
    return 0;
}

static char **mrl_complete(microrl_t *mrl, int argc, const char *const *argv)
{
    int32_t j = 0;

    if (argc == 1)
    {
        for (int32_t i = 0; command_table[i].name != NULL; i++)
        {
            char *name = command_table[i].name;
            if (strstr(name, argv[0]) == name)
            {
                complete_table[j++] = name;
            }
        }
    }
    else if (argc == 0)
    {
        for (int32_t i = 0; command_table[i].name != NULL; i++)
        {
            char *name = command_table[i].name;
            complete_table[j++] = name;
        }
    }

    complete_table[j] = NULL;

    return complete_table;
}

static void shell(void)
{
    microrl_t rl;

    microrl_init(&rl, mrl_print, mrl_execute);
    microrl_set_complete_callback(&rl, mrl_complete);

    while (true)
    {
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT)
        {
            char ch = (char)c;
            microrl_processing_input(&rl, &ch, 1);
        }
    }
}


int main(void)
{
    bool config_ok;

    // vreg_set_voltage(VREG_VOLTAGE_1_10); // VREG_VOLTAGE_DEFAULT
    // vreg_set_voltage(VREG_VOLTAGE_1_25);
    vreg_set_voltage(VREG_VOLTAGE_MAX); // 1_30
    // set_sys_clock_khz(250000, true);
    // set_sys_clock_khz(320000, true);
    // set_sys_clock_khz(400000, true);
    set_sys_clock_khz(CPU_CLOCK_FREQ_NORMAL, true);

    gpio_init_mask(GPIO_ALL_MASK);
    stdio_init_all();

    config_ok = config_load();
    if (!config_ok)
    {
        printf("configration is broken. initialize.\n");
        config_init();
        config_save_init();
    }

    {
        int32_t ch = rom_load_async_start(config.cfg.rom_bank);
        ram_clear();
        rom_load_async_wait(ch);
    }

    // Overclocking
    set_sys_clock_khz(CPU_CLOCK_FREQ_HIGH, true);

#ifdef DEBUG_PULL_UP
    {
        // for debugging with logic analyzer
        for (uint pin = GPIO_ADDR; pin < GPIO_ADDR_END; pin++)
        {
            gpio_pull_down(pin);
        }
        // 0x2080
        gpio_pull_up(GPIO_ADDR + 7);
        gpio_pull_up(GPIO_ADDR + 13);
        for (uint pin = GPIO_DATA; pin < GPIO_DATA_END; pin++)
        {
            gpio_pull_down(pin);
        }
    }
    gpio_pull_up(GPIO_CE);
    gpio_pull_up(GPIO_OE);
    gpio_pull_up(GPIO_WR);
#endif

    gpio_config = config.cfg.gpio_config;
    {
        // only for ext0-2
        gpio_set_dir_out_masked(gpio_config.dir & GPIO_EXT_MASK);
        gpio_set_dir_in_masked(~gpio_config.dir & GPIO_EXT_MASK);
        gpio_set_mask(gpio_config.value & GPIO_EXT_MASK);
        gpio_clr_mask(~gpio_config.value & GPIO_EXT_MASK);

        for (uint32_t pin = 0; pin < GPIO_END; pin++)
        {
            if (gpio_config.pullup & (1 << pin))
            {
                gpio_pull_up(pin);
            }
            if (gpio_config.pulldown & (1 << pin))
            {
                gpio_pull_down(pin);
            }
        }
    }

    if (config.cfg.mode == CONFIG_MODE_EMULATOR)
    {
        romemu_init(pio0, 0, rom);
        busmon_init(pio1, 0, ram);

        memcpy(capture_target, config.cfg.capture_target, sizeof(capture_target));

        command_table = command_table_emulator;
        multicore_launch_core1(core1_entry_emulator);
    }
    else if (config.cfg.mode == CONFIG_MODE_CLONE)
    {
        gpio_set_dir_out_masked(GPIO_ADDR_MASK | GPIO_CE_MASK | GPIO_OE_MASK);
        gpio_set_dir_in_masked(GPIO_DATA_MASK);
        for (uint pin = GPIO_ADDR; pin < GPIO_ADDR_END; pin++)
        {
            gpio_pull_up(pin);
            gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_12MA);
        }
        for (uint pin = GPIO_DATA; pin < GPIO_DATA_END; pin++)
        {
            gpio_pull_up(pin);
        }
        gpio_pull_up(GPIO_CE);
        gpio_set_drive_strength(GPIO_CE, GPIO_DRIVE_STRENGTH_12MA);
        gpio_pull_up(GPIO_OE);
        gpio_set_drive_strength(GPIO_OE, GPIO_DRIVE_STRENGTH_12MA);

        command_table = command_table_clone;
        multicore_launch_core1(core1_entry_clone);
    }

    while (!tud_cdc_connected())
        sleep_ms(100);
    sleep_ms(250);

    printf("\n");
    printf("connected.\n");
    printf("%s\n", MAGIC_STR);
    printf("rom bank: %d\n", config.cfg.rom_bank);
    switch (config.cfg.mode)
    {
    case CONFIG_MODE_EMULATOR:
        printf("mode: emulator\n");
        break;
    case CONFIG_MODE_CLONE:
        printf("mode: clone\n");
        break;
    default:
        printf("mode: unknown (%d)\n", config.cfg.mode);
        break;
    }

    shell();

    return 0;
}
