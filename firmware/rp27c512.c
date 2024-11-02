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
#include "pico/stdlib.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tusb.h>

#include "microrl.h"
#include "xmodem.h"

#include "busmon.h"
#include "romemu.h"


#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

//                /0123456789ABCDEF
#define MAGIC_STR "RP27C512 VER1.00"
#define MAGIC_SIZE (16)

// #define PULL_UP
#define GPIO_ADDR 0
#define GPIO_ADDR_END 16
#define GPIO_DATA 16 // 0-7
#define GPIO_DATA_END 24
#define GPIO_EXT0 24
#define GPIO_EXT1 25
#define GPIO_EXT2 26
#define GPIO_CS 27
#define GPIO_OE 28
#define GPIO_WR 29
#define GPIO_END 30

#define GPIO_ALL_MASK ((1 << GPIO_END) - 1)
#define GPIO_ADDR_MASK (((1 << (GPIO_ADDR_END - GPIO_ADDR)) - 1) << GPIO_ADDR)
#define GPIO_DATA_MASK (((1 << (GPIO_DATA_END - GPIO_DATA)) - 1) << GPIO_DATA)
#define GPIO_CS_MASK (1 << GPIO_CS)
#define GPIO_OE_MASK (1 << GPIO_OE)

#define GPIO_GET_DATA(v) (((v) & GPIO_DATA_MASK) >> GPIO_DATA)


#define CPU_CLOCK_FREQ_HIGH (400 * 1000)
#define CPU_CLOCK_FREQ_NORMAL (125 * 1000)
#define REBOOT_DELAY_MS (250)
#define FLASH_WAIT_MS (500)


static uint8_t rom[0x10000] __attribute__((aligned(0x10000))) = {0};
static uint8_t ram[0x10000] __attribute__((aligned(0x10000))) = {0};
static uint8_t *device = rom;

static uint8_t init_rom_data[FLASH_PAGE_SIZE];

#define CAPTURE_COUNT 8192
static uint32_t capture_buffer[CAPTURE_COUNT];
static volatile uint32_t capture_wp = 0;
static uint32_t capture_rp = 0;
static bool capture_target_bank[0x100];

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
    char            magic[MAGIC_SIZE];
    config_mode_e   mode;
    int32_t         rom_bank;
} config_t;

static union
{
    config_t cfg;
    uint8_t bin[FLASH_PAGE_SIZE];
} config;

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
    flash_range_erase(FLASH_TARGET_OFFSET_CONFIG, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET_CONFIG, config.bin, sizeof(config));
    multicore_lockout_end_blocking();
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
    gpio_put_all(bit(GPIO_CS) | bit(GPIO_OE));
    sleep_us(1);
    for (uint32_t addr = start; addr < end; addr++)
    {
        gpio_put_all(addr | bit(GPIO_OE));
        sleep_us(1);
        gpio_put_all(addr);
        sleep_us(1);
        dst[addr] = GPIO_GET_DATA(gpio_get_all());
    }
    gpio_put_all(bit(GPIO_CS) | bit(GPIO_OE));
}

static bool clone(void)
{
    read_rom(rom, 0x0000, 0x10000);
    read_rom(ram, 0x0000, 0x10000);
    return memcmp(rom, ram, sizeof(rom)) == 0;
}

static void core1_entry_emulator(void)
{
    uint32_t cap;
    uint32_t rw = 3;
    uint32_t rw_prev;
    uint32_t addr;
    uint32_t data;

    multicore_lockout_victim_init();

    busmon_cap_start();
    while (true)
    {
        if (!busmon_cap_is_empty())
        {
            cap = busmon_cap_pop();
            addr = cap & 0x00ffff;
            if (capture_target_bank[addr >> 8])
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

static void memdump(const uint8_t *mem, uint32_t addr)
{
    for (int y = 0; y < 16; y++)
    {
        printf("%04x ", addr);
        for (int x = 0; x < 16; x++)
        {
            printf(" %02x", mem[addr + x]);
        }
        printf("\n");
        addr += 16;
    }
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

static void cmd_gpio(int argc, const char *const *argv)
{
    printf("GPIO: %08x\n", gpio_get_all());
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
    memdump(device, addr);
    addr += 0x100;
}

static void cmd_dump_watch(int argc, const char *const *argv)
{
    static uint32_t addr = 0;
    printf("\x1b[2J");
    if (argc > 1)
    {
        addr = strtol(argv[1], NULL, 16);
    }
    while (getchar_timeout_us(0) == PICO_ERROR_TIMEOUT)
    {
        printf("\x1b[0;0H");
        memdump(device, addr);
    }
    addr += 0x100;
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

    printf("bank 0,1,2,3\n");
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

    printf("erase rom bank %d ... ", bank);
    ret = rom_erase_slow(bank);
    printf("done.\n");

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
    uint32_t wait_s = 5;
    int32_t verify_num = 2;

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
            clone_ok = false;
        }
    }
    printf("clone: %s\n", clone_ok ? "OK" : "NG");
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
    {"gpio",    cmd_gpio,       "show GPIO status"},

    {"device",  cmd_device,     "select device (device rom|ram)"},
    {"d",       cmd_dump,       "dump device (d address)"},
    {"dw",      cmd_dump_watch, "dump device repeatly (dw address)"},
    {"cap",     cmd_capture,    "show capture log"},

    {"recv",    cmd_recv,       "receive data from host (XMODEM CRC)"},
    {"send",    cmd_send,       "send data to host (XMODEM 1K)"},

    {"bank",    cmd_bank,       "select flash rom bank (bank 0,1,2,3)"},
    {"load",    cmd_load,       "load data from current flash rom bank"},
    {"save",    cmd_save,       "save data to current flash rom bank"},
    {"erase",   cmd_erase,      "erase flash rom bank"},

    {NULL, NULL}
};

static const command_table_t command_table_clone[] =
{
    {"help",    cmd_help,       "show help"},
    {"?",       cmd_help,       "show help"},

    {"hello",   cmd_hello,      "test: hello, world"},

    {"reboot",  cmd_reboot,     "reboot RP27C512"},
    {"mode",    cmd_mode,       "select mode (mode emulator|clone)"},
    {"gpio",    cmd_gpio,       "show GPIO status"},

    {"d",       cmd_dump,       "dump device (d address)"},

    {"recv",    cmd_recv,       "receive data from host (XMODEM CRC)"},
    {"send",    cmd_send,       "send data to host (XMODEM 1K)"},

    {"bank",    cmd_bank,       "select flash rom bank (bank 0,1,2,3)"},
    {"load",    cmd_load,       "load data from current flash rom bank"},
    {"save",    cmd_save,       "save data to current flash rom bank"},
    {"erase",   cmd_erase,      "erase flash rom bank"},

    {"clone",   cmd_clone,      "clone from real ROM chip (clone wait verify_num)"},

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

    gpio_init_mask(GPIO_ALL_MASK);
    stdio_init_all();

    config_ok = config_load();
    if (!config_ok)
    {
        printf("configration is broken. initialize.\n");
        config_init();
        config_save();
    }

    {
        int32_t ch = rom_load_async_start(config.cfg.rom_bank);
        ram_clear();
        rom_load_async_wait(ch);
    }

    // vreg_set_voltage(VREG_VOLTAGE_1_10); // VREG_VOLTAGE_DEFAULT
    // vreg_set_voltage(VREG_VOLTAGE_1_25);
    vreg_set_voltage(VREG_VOLTAGE_MAX); // 1_30
    // set_sys_clock_khz(250000, true);
    // set_sys_clock_khz(320000, true);
    // set_sys_clock_khz(400000, true);
    set_sys_clock_khz(CPU_CLOCK_FREQ_HIGH, true);

#ifdef PULL_UP
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
    gpio_pull_up(GPIO_CS);
    gpio_pull_up(GPIO_OE);
    gpio_pull_up(GPIO_WR);
#endif

    {
        // for future use
        gpio_pull_down(GPIO_EXT0);
        gpio_pull_down(GPIO_EXT1);
        gpio_pull_down(GPIO_EXT2);
    }

    if (config.cfg.mode == CONFIG_MODE_EMULATOR)
    {
        for (int32_t i = 0; i < 0x100; i++)
        {
            capture_target_bank[i] = false;
        }
        capture_target_bank[0x10] = true;
        capture_target_bank[0x14] = true;

        romemu_init(pio0, 0, rom);
        busmon_init(pio1, 0, ram);

        multicore_launch_core1(core1_entry_emulator);
    }
    else if (config.cfg.mode == CONFIG_MODE_CLONE)
    {
        gpio_set_dir_out_masked(GPIO_ADDR_MASK | GPIO_CS_MASK | GPIO_OE_MASK);
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
        gpio_pull_up(GPIO_CS);
        gpio_set_drive_strength(GPIO_CS, GPIO_DRIVE_STRENGTH_12MA);
        gpio_pull_up(GPIO_OE);
        gpio_set_drive_strength(GPIO_OE, GPIO_DRIVE_STRENGTH_12MA);

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
        command_table = command_table_emulator;
        break;
    case CONFIG_MODE_CLONE:
        printf("mode: clone\n");
        command_table = command_table_clone;
        break;
    default:
        printf("mode: unknown (%d)\n", config.cfg.mode);
        break;
    }

    shell();

    return 0;
}
