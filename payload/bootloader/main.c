/*
 * Copyright (c) 2018 naehrwert
 *
 * Copyright (c) 2018 CTCaer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <stdlib.h>

#include "config/config.h"
#include "gfx/di.h"
#include "gfx/gfx.h"
#include "gfx/logos.h"
#include "gfx/tui.h"
#include "hos/hos.h"
#include "ianos/ianos.h"
#include "libs/compr/blz.h"
#include "libs/fatfs/ff.h"
#include "mem/heap.h"
#include "mem/sdram.h"
#include "power/max77620.h"
#include "soc/hw_init.h"
#include "soc/i2c.h"
#include "soc/pmc.h"
#include "soc/t210.h"
#include "soc/uart.h"
#include "storage/sdmmc.h"
#include "utils/btn.h"
#include "utils/dirlist.h"
#include "utils/list.h"
#include "utils/util.h"

#include "frontend/fe_emmc_tools.h"
#include "frontend/fe_tools.h"
#include "frontend/fe_info.h"

//TODO: ugly.
gfx_ctxt_t gfx_ctxt;
gfx_con_t gfx_con;

//TODO: Create more macros (info, header, debug, etc) with different colors and utilize them for consistency.
#define EPRINTF(text) gfx_printf(&gfx_con, "%k"text"%k\n", 0xFFFF0000, 0xFFCCCCCC)
#define EPRINTFARGS(text, args...) gfx_printf(&gfx_con, "%k"text"%k\n", 0xFFFF0000, args, 0xFFCCCCCC)
#define WPRINTF(text) gfx_printf(&gfx_con, "%k"text"%k\n", 0xFFFFDD00, 0xFFCCCCCC)
#define WPRINTFARGS(text, args...) gfx_printf(&gfx_con, "%k"text"%k\n", 0xFFFFDD00, args, 0xFFCCCCCC)

//TODO: ugly.
sdmmc_t sd_sdmmc;
sdmmc_storage_t sd_storage;
FATFS sd_fs;
static bool sd_mounted;

hekate_config h_cfg;

bool sd_mount()
{
	if (sd_mounted)
		return true;

	if (!sdmmc_storage_init_sd(&sd_storage, &sd_sdmmc, SDMMC_1, SDMMC_BUS_WIDTH_4, 11))
	{
		EPRINTF("Failed to init SD card.\nMake sure that it is inserted.\nOr that SD reader is properly seated!");
	}
	else
	{
		int res = 0;
		res = f_mount(&sd_fs, "", 1);
		if (res == FR_OK)
		{
			sd_mounted = 1;
			return true;
		}
		else
		{
			EPRINTFARGS("Failed to mount SD card (FatFS Error %d).\nMake sure that a FAT partition exists..", res);
		}
	}

	return false;
}

void sd_unmount()
{
	if (sd_mounted)
	{
		f_mount(NULL, "", 1);
		sdmmc_storage_end(&sd_storage);
		sd_mounted = false;
	}
}

void *sd_file_read(char *path)
{
	FIL fp;
	if (f_open(&fp, path, FA_READ) != FR_OK)
		return NULL;

	u32 size = f_size(&fp);
	void *buf = malloc(size);

	u8 *ptr = buf;
	while (size > 0)
	{
		u32 rsize = MIN(size, 512 * 512);
		if (f_read(&fp, ptr, rsize, NULL) != FR_OK)
		{
			free(buf);
			return NULL;
		}

		ptr += rsize;
		size -= rsize;
	}

	f_close(&fp);

	return buf;
}

int sd_save_to_file(void *buf, u32 size, const char *filename)
{
	FIL fp;
	u32 res = 0;
	res = f_open(&fp, filename, FA_CREATE_ALWAYS | FA_WRITE);
	if (res)
	{
		EPRINTFARGS("Error (%d) creating file\n%s.\n", res, filename);
		return 1;
	}

	f_sync(&fp);
	f_write(&fp, buf, size, NULL);
	f_close(&fp);

	return 0;
}


void panic(u32 val)
{
	// Set panic code.
	PMC(APBDEV_PMC_SCRATCH200) = val;
	//PMC(APBDEV_PMC_CRYPTO_OP) = 1; // Disable SE.
	TMR(TIMER_WDT4_UNLOCK_PATTERN) = TIMER_MAGIC_PTRN;
	TMR(TIMER_TMR9_TMR_PTV) = TIMER_EN | TIMER_PER_EN;
	TMR(TIMER_WDT4_CONFIG)  = TIMER_SRC(9) | TIMER_PER(1) | TIMER_PMCRESET_EN;
	TMR(TIMER_WDT4_COMMAND) = TIMER_START_CNT;
	while (1)
		;
}



void power_off()
{
	sd_unmount();
	//TODO: we should probably make sure all regulators are powered off properly.
	i2c_send_byte(I2C_5, MAX77620_I2C_ADDR, MAX77620_REG_ONOFFCNFG1, MAX77620_ONOFFCNFG1_PWR_OFF);
}



// This is a safe and unused DRAM region for our payloads.
#define IPL_LOAD_ADDR      0x40008000
#define EXT_PAYLOAD_ADDR   0xC03C0000
#define PATCHED_RELOC_SZ   0x94
#define RCM_PAYLOAD_ADDR   (EXT_PAYLOAD_ADDR + ALIGN(PATCHED_RELOC_SZ, 0x10))
#define PAYLOAD_ENTRY      0x40010000
#define CBFS_SDRAM_EN_ADDR 0x4003e000
#define COREBOOT_ADDR      (0xD0000000 - 0x100000)

void (*ext_payload_ptr)() = (void *)EXT_PAYLOAD_ADDR;
void (*update_ptr)() = (void *)RCM_PAYLOAD_ADDR;

void reloc_patcher(u32 payload_size)
{
	static const u32 START_OFF = 0x7C;
	static const u32 PAYLOAD_END_OFF = 0x84;
	static const u32 IPL_START_OFF = 0x88;

	memcpy((u8 *)EXT_PAYLOAD_ADDR, (u8 *)IPL_LOAD_ADDR, PATCHED_RELOC_SZ);

	*(vu32 *)(EXT_PAYLOAD_ADDR + START_OFF) = PAYLOAD_ENTRY - ALIGN(PATCHED_RELOC_SZ, 0x10);
	*(vu32 *)(EXT_PAYLOAD_ADDR + PAYLOAD_END_OFF) = PAYLOAD_ENTRY + payload_size;
	*(vu32 *)(EXT_PAYLOAD_ADDR + IPL_START_OFF) = PAYLOAD_ENTRY;

	if (payload_size == 0x7000)
	{
		memcpy((u8 *)(EXT_PAYLOAD_ADDR + ALIGN(PATCHED_RELOC_SZ, 0x10)), (u8 *)COREBOOT_ADDR, 0x7000); //Bootblock
		*(vu32 *)CBFS_SDRAM_EN_ADDR = 0x4452414D;
	}
}

#define BOOTLOADER_UPDATED_MAGIC      0x424F4F54 // "BOOT".
#define BOOTLOADER_UPDATED_MAGIC_ADDR 0x4003E000





extern void pivot_stack(u32 stack_top);

void ipl_main()
{
	config_hw();

	//Pivot the stack so we have enough space.
	pivot_stack(0x90010000);

	//Tegra/Horizon configuration goes to 0x80000000+, package2 goes to 0xA9800000, we place our heap in between.
	heap_init(0x90020000);


	// Set bootloader's default configuration.
	set_default_configuration();

	// Save sdram lp0 config.
	//ianos_loader(true, "bootloader/sys/libsys_lp0.bso", DRAM_LIB, (void *)sdram_get_params_patched());

	display_init();

	u32 *fb = display_init_framebuffer();
	gfx_init_ctxt(&gfx_ctxt, fb, 720, 1280, 720);


	gfx_con_init(&gfx_con, &gfx_ctxt);

	display_backlight_pwm_init();

	gfx_con.mute = true;

	//sd_unmount();
	gfx_clear_grey(&gfx_ctxt, 0x1B);
	u8 *BOOTLOGO = (void *)malloc(0x4000);
	blz_uncompress_srcdest(BOOTLOGO_BLZ, SZ_BOOTLOGO_BLZ, BOOTLOGO, SZ_BOOTLOGO);
	gfx_set_rect_grey(&gfx_ctxt, BOOTLOGO, X_BOOTLOGO, Y_BOOTLOGO, 326, 544);

	display_backlight_brightness(10, 5000);
	display_backlight_brightness(100, 25000);
	usleep(600000);
	display_backlight_brightness(0, 20000);
	
	power_off();


}
