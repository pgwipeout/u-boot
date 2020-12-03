/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright (c) 2020, Peter Geis <pgwipeout@gmail.com>
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include <linux/sizes.h>

#include "tegra30-common.h"

/* VDD core PMIC */
#define CONFIG_TEGRA_VDD_CORE_TPS62361B_SET3

/* High-level configuration options */
#define CONFIG_TEGRA_BOARD_STRING	"Ouya Game Console"

#define BOARD_EXTRA_ENV_SETTINGS \
        "reboot_bootloader=mw 7000e450 40000000 ; mw 7000e400 10\0" \
        "reboot_recovery=mw 7000e450 80000000 ; mw 7000e400 10\0" \
        "reboot_forced_recovery=mw 7000e450 2 ; mw 7000e400 10\0" \
	"devnum=0\0"  \
	"devtype=mmc\0" \
	"distro_bootpart=3\0" \
	"bootcmd_ouya=run scan_dev_for_boot\0"

#define FORCE_MANUAL_GPT
#define GPT_PRIMARY_PARTITION_TABLE_LBA 15073279ULL
/* Board-specific serial config */
#define CONFIG_TEGRA_ENABLE_UARTD
#define CONFIG_SYS_NS16550_COM1		NV_PA_APB_UARTD_BASE

#define CONFIG_MACH_TYPE		MACH_TYPE_CARDHU

/* SPI */
#define CONFIG_TEGRA_SLINK_CTRLS       6
#define CONFIG_SPI_FLASH_SIZE          (4 << 20)

#include "tegra-common-usb-gadget.h"
#include "tegra-common-post.h"

#ifdef CONFIG_TARGET_OUYA_CHAINLOAD
#define CONFIG_SKIP_LOWLEVEL_INIT
#define CONFIG_PRAM 4096
#endif /* CONFIG_TARGET_OUYA_CHAINLOAD */

#endif /* __CONFIG_H */
