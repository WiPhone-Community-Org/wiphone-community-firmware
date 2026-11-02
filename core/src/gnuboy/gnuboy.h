#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef RETRO_GO
#include <rg_system.h>
#define LOG_PRINTF(level, x...) rg_system_log(RG_LOG_PRINTF, NULL, x)
#else
#define LOG_PRINTF(level, x...) printf(x)
// Real IRAM_ATTR (the old empty fallback silently discarded IRAM placement).
// WiPhone has ~9.6KB of free IRAM: enough for the memory-bus and scanline hot
// paths below, NOT for the 14KB gb_cpu_emulate (see cpu.c).
#include "esp_attr.h"
#endif

#define MESSAGE_ERROR(x, ...) LOG_PRINTF(1, "!! %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_WARN(x, ...)  LOG_PRINTF(2, "** %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_INFO(x, ...)  LOG_PRINTF(3, " * %s: " x, __func__, ## __VA_ARGS__)
// #define MESSAGE_DEBUG(x, ...) LOG_PRINTF(4, ">> %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_DEBUG(x, ...) {}

#define GB_WIDTH (160)
#define GB_HEIGHT (144)

typedef uint8_t byte;

typedef enum
{
	GB_HW_DMG,
	GB_HW_CGB,
	GB_HW_SGB,
} gb_hwtype_t;

typedef enum
{
	GB_PAD_RIGHT = 0x01,
	GB_PAD_LEFT = 0x02,
	GB_PAD_UP = 0x04,
	GB_PAD_DOWN = 0x08,
	GB_PAD_A = 0x10,
	GB_PAD_B = 0x20,
	GB_PAD_SELECT = 0x40,
	GB_PAD_START = 0x80,
} gb_padbtn_t;

typedef enum
{
	GB_PALETTE_0,
	GB_PALETTE_1,
	GB_PALETTE_2,
	GB_PALETTE_3,
	GB_PALETTE_4,
	GB_PALETTE_5,
	GB_PALETTE_6,
	GB_PALETTE_7,
	GB_PALETTE_8,
	GB_PALETTE_9,
	GB_PALETTE_10,
	GB_PALETTE_11,
	GB_PALETTE_12,
	GB_PALETTE_13,
	GB_PALETTE_14,
	GB_PALETTE_15,
	GB_PALETTE_16,
	GB_PALETTE_17,
	GB_PALETTE_18,
	GB_PALETTE_19,
	GB_PALETTE_20,
	GB_PALETTE_21,
	GB_PALETTE_22,
	GB_PALETTE_23,
	GB_PALETTE_24,
	GB_PALETTE_25,
	GB_PALETTE_26,
	GB_PALETTE_27,
	GB_PALETTE_28,
	GB_PALETTE_29,
	GB_PALETTE_30,
	GB_PALETTE_31,
	GB_PALETTE_DMG,
	GB_PALETTE_MGB0,
	GB_PALETTE_MGB1,
	GB_PALETTE_CGB,
	GB_PALETTE_SGB,
	GB_PALETTE_COUNT,
} gb_palette_t;

typedef enum
{
	GB_PIXEL_PALETTED,
	GB_PIXEL_565_LE,
	GB_PIXEL_565_BE,
} gb_video_fmt_t;

typedef enum
{
	GB_AUDIO_STEREO_S16,
	GB_AUDIO_MONO_S16,
} gb_audio_fmt_t;

typedef void (gb_video_cb_t)(void *buffer);
typedef void (gb_audio_cb_t)(void *buffer, size_t length);

int  gnuboy_init(int samplerate, gb_audio_fmt_t audio_fmt, gb_video_fmt_t video_fmt, gb_video_cb_t *video_callback, gb_audio_cb_t *audio_callback);
void gnuboy_deinit(void);   // free WRAM/VRAM; gnuboy_init() may be called again later
int  gnuboy_load_bios(const byte *data, size_t size);
int  gnuboy_load_bios_file(const char *file);
void gnuboy_free_bios(void);
int  gnuboy_load_rom(const byte *data, size_t size);
int  gnuboy_load_rom_file(const char *file);
void gnuboy_free_rom(void);
void gnuboy_reset(bool hard);
void gnuboy_run(bool draw);
bool gnuboy_sram_dirty(void);
void gnuboy_load_bank(int);
void gnuboy_set_pad(int);

void gnuboy_set_framebuffer(void *buffer);
void gnuboy_set_soundbuffer(void *buffer, size_t length);
size_t gnuboy_get_audio_count(void);   // int16 samples the last gnuboy_run() produced
void gnuboy_set_io_lock(void (*lock)(void), void (*unlock)(void));   // guards SD bank reads

void gnuboy_get_time(int *day, int *hour, int *minute, int *second);
void gnuboy_set_time(int day, int hour, int minute, int second);
int  gnuboy_get_hwtype(void);
void gnuboy_set_hwtype(gb_hwtype_t type);
int  gnuboy_get_palette(void);
void gnuboy_set_palette(gb_palette_t pal);

int gnuboy_load_sram(const char *file);
int gnuboy_save_sram(const char *file, bool quick_save);
int gnuboy_load_state(const char *file);
int gnuboy_save_state(const char *file);
/* The exact byte size gnuboy_save_state writes for the LOADED cart (header block + WRAM +
 * VRAM + cart RAM, 4 KB blocks). do_save_load's fwrite/fread checks only catch a call that
 * moved NOTHING (`< 1`), so a short write on a full card or a truncated file both pass as
 * success; the app compares the file's size with this instead (app_gbc.cpp writeState /
 * loadState). 0 when no cart is loaded. */
size_t gnuboy_state_size(void);
/* Identity of the LOADED cart for naming its state files: the header's global checksum
 * (0x14E-0x14F) with the ROM and RAM size codes. Two ROMs that sanitize to the same file
 * name (a hack, a revision, a re-upload under the same name) get different states, and a
 * state can never be resumed into a cart it was not written from. 0 when nothing is loaded. */
uint32_t gnuboy_cart_id(void);

#ifdef __cplusplus
}
#endif
