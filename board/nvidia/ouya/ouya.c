// SPDX-License-Identifier: GPL-2.0+
/*
 *  Copyright (c) 2020, Peter Geis <pgwipeout@gmail.com>
 */

#include <common.h>
#include <asm/arch/pinmux.h>
#include <asm/arch/gp_padctrl.h>
#include <asm/arch/gpio.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include "pinmux-config-ouya.h"

void pinmux_init(void)
{
	pr_warn("called pinmux_init\n");
	pinmux_config_pingrp_table(tegra3_pinmux_common,
		ARRAY_SIZE(tegra3_pinmux_common));

	pinmux_config_pingrp_table(unused_pins_lowpower,
		ARRAY_SIZE(unused_pins_lowpower));

	/* Initialize any non-default pad configs (APB_MISC_GP regs) */
//	pinmux_config_drvgrp_table(cardhu_padctrl, ARRAY_SIZE(cardhu_padctrl));
}

void pin_mux_mmc(void)
{
	pr_warn("called pinmux_init\n");
	pinmux_config_drvgrp_table(cardhu_padctrl, ARRAY_SIZE(cardhu_padctrl));
//	writel(0x334A4000, 0x70000900);
//	writel(0x334A4000, 0x70000904);
//	writel(0x334A4000, 0x70000908);
//	writel(0x334A4000, 0x7000090C);
}

void start_cpu_fan(void)
{
	/* GPIO_PJ2 is FAN_ENABLE */
	gpio_request(TEGRA_GPIO(J, 2), "FAN_VDD");
	gpio_direction_output(TEGRA_GPIO(J, 2), 1);
}

//int arch_misc_init(void)
//{
	/* GPIO_PDD4 is PS_VDD_FAN_ENABLE */
//	gpio_request(TEGRA_GPIO(DD, 4), "USB_VDD");
//	gpio_direction_output(TEGRA_GPIO(DD, 4), 1);

//	return 0;
//}
