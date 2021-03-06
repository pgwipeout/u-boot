// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * (C) Copyright 2016-2017 Rockchip Inc.
 *
 * Adapted from coreboot.
 */

#include <common.h>
#include <clk.h>
#include <dm.h>
#include <dt-structs.h>
#include <ram.h>
#include <regmap.h>
#include <syscon.h>
#include <asm/io.h>
#include <asm/arch-rockchip/clock.h>
#include <asm/arch-rockchip/cru_rk3399.h>
#include <asm/arch-rockchip/grf_rk3399.h>
#include <asm/arch-rockchip/pmu_rk3399.h>
#include <asm/arch-rockchip/hardware.h>
#include <asm/arch-rockchip/sdram_common.h>
#include <asm/arch-rockchip/sdram_rk3399.h>
#include <linux/err.h>
#include <time.h>

#define PRESET_SGRF_HOLD(n)	((0x1 << (6 + 16)) | ((n) << 6))
#define PRESET_GPIO0_HOLD(n)	((0x1 << (7 + 16)) | ((n) << 7))
#define PRESET_GPIO1_HOLD(n)	((0x1 << (8 + 16)) | ((n) << 8))

#define PHY_DRV_ODT_HI_Z	0x0
#define PHY_DRV_ODT_240		0x1
#define PHY_DRV_ODT_120		0x8
#define PHY_DRV_ODT_80		0x9
#define PHY_DRV_ODT_60		0xc
#define PHY_DRV_ODT_48		0xd
#define PHY_DRV_ODT_40		0xe
#define PHY_DRV_ODT_34_3	0xf

#define PHY_BOOSTP_EN		0x1
#define PHY_BOOSTN_EN		0x1
#define PHY_SLEWP_EN		0x1
#define PHY_SLEWN_EN		0x1
#define PHY_RX_CM_INPUT		0x1
#define CS0_MR22_VAL		0
#define CS1_MR22_VAL		3

#define CRU_SFTRST_DDR_CTRL(ch, n)	((0x1 << (8 + 16 + (ch) * 4)) | \
					((n) << (8 + (ch) * 4)))
#define CRU_SFTRST_DDR_PHY(ch, n)	((0x1 << (9 + 16 + (ch) * 4)) | \
					((n) << (9 + (ch) * 4)))
struct chan_info {
	struct rk3399_ddr_pctl_regs *pctl;
	struct rk3399_ddr_pi_regs *pi;
	struct rk3399_ddr_publ_regs *publ;
	struct rk3399_msch_regs *msch;
};

struct dram_info {
#if defined(CONFIG_TPL_BUILD) || \
	(!defined(CONFIG_TPL) && defined(CONFIG_SPL_BUILD))
	u32 pwrup_srefresh_exit[2];
	struct chan_info chan[2];
	struct clk ddr_clk;
	struct rk3399_cru *cru;
	struct rk3399_grf_regs *grf;
	struct rk3399_pmu_regs *pmu;
	struct rk3399_pmucru *pmucru;
	struct rk3399_pmusgrf_regs *pmusgrf;
	struct rk3399_ddr_cic_regs *cic;
	const struct sdram_rk3399_ops *ops;
#endif
	struct ram_info info;
	struct rk3399_pmugrf_regs *pmugrf;
};

struct sdram_rk3399_ops {
	int (*data_training)(struct dram_info *dram, u32 channel, u8 rank,
			     struct rk3399_sdram_params *sdram);
	int (*set_rate)(struct dram_info *dram,
			struct rk3399_sdram_params *params);
};

#if defined(CONFIG_TPL_BUILD) || \
	(!defined(CONFIG_TPL) && defined(CONFIG_SPL_BUILD))

struct rockchip_dmc_plat {
#if CONFIG_IS_ENABLED(OF_PLATDATA)
	struct dtd_rockchip_rk3399_dmc dtplat;
#else
	struct rk3399_sdram_params sdram_params;
#endif
	struct regmap *map;
};

struct io_setting {
	u32 mhz;
	u32 mr5;
	/* dram side */
	u32 dq_odt;
	u32 ca_odt;
	u32 pdds;
	u32 dq_vref;
	u32 ca_vref;
	/* phy side */
	u32 rd_odt;
	u32 wr_dq_drv;
	u32 wr_ca_drv;
	u32 wr_ckcs_drv;
	u32 rd_odt_en;
	u32 rd_vref;
} lpddr4_io_setting[] = {
	{
		50 * MHz,
		0,
		/* dram side */
		0,	/* dq_odt; */
		0,	/* ca_odt; */
		6,	/* pdds; */
		0x72,	/* dq_vref; */
		0x72,	/* ca_vref; */
		/* phy side */
		PHY_DRV_ODT_HI_Z,	/* rd_odt; */
		PHY_DRV_ODT_40,	/* wr_dq_drv; */
		PHY_DRV_ODT_40,	/* wr_ca_drv; */
		PHY_DRV_ODT_40,	/* wr_ckcs_drv; */
		0,	/* rd_odt_en;*/
		41,	/* rd_vref; (unit %, range 3.3% - 48.7%) */
	},
	{
		600 * MHz,
		0,
		/* dram side */
		1,	/* dq_odt; */
		0,	/* ca_odt; */
		6,	/* pdds; */
		0x72,	/* dq_vref; */
		0x72,	/* ca_vref; */
		/* phy side */
		PHY_DRV_ODT_HI_Z,	/* rd_odt; */
		PHY_DRV_ODT_48,	/* wr_dq_drv; */
		PHY_DRV_ODT_40,	/* wr_ca_drv; */
		PHY_DRV_ODT_40,	/* wr_ckcs_drv; */
		0,	/* rd_odt_en; */
		32,	/* rd_vref; (unit %, range 3.3% - 48.7%) */
	},
	{
		800 * MHz,
		0,
		/* dram side */
		1,	/* dq_odt; */
		0,	/* ca_odt; */
		1,	/* pdds; */
		0x72,	/* dq_vref; */
		0x72,	/* ca_vref; */
		/* phy side */
		PHY_DRV_ODT_40,	/* rd_odt; */
		PHY_DRV_ODT_48,	/* wr_dq_drv; */
		PHY_DRV_ODT_40,	/* wr_ca_drv; */
		PHY_DRV_ODT_40,	/* wr_ckcs_drv; */
		1,	/* rd_odt_en; */
		17,	/* rd_vref; (unit %, range 3.3% - 48.7%) */
	},
	{
		933 * MHz,
		0,
		/* dram side */
		3,	/* dq_odt; */
		0,	/* ca_odt; */
		6,	/* pdds; */
		0x59,	/* dq_vref; 32% */
		0x72,	/* ca_vref; */
		/* phy side */
		PHY_DRV_ODT_HI_Z,	/* rd_odt; */
		PHY_DRV_ODT_48,	/* wr_dq_drv; */
		PHY_DRV_ODT_40,	/* wr_ca_drv; */
		PHY_DRV_ODT_40,	/* wr_ckcs_drv; */
		0,	/* rd_odt_en; */
		32,	/* rd_vref; (unit %, range 3.3% - 48.7%) */
	},
	{
		1066 * MHz,
		0,
		/* dram side */
		6,	/* dq_odt; */
		0,	/* ca_odt; */
		1,	/* pdds; */
		0x10,	/* dq_vref; */
		0x72,	/* ca_vref; */
		/* phy side */
		PHY_DRV_ODT_40,	/* rd_odt; */
		PHY_DRV_ODT_60,	/* wr_dq_drv; */
		PHY_DRV_ODT_40,	/* wr_ca_drv; */
		PHY_DRV_ODT_40,	/* wr_ckcs_drv; */
		1,	/* rd_odt_en; */
		17,	/* rd_vref; (unit %, range 3.3% - 48.7%) */
	},
};

/**
 * phy = 0, PHY boot freq
 * phy = 1, PHY index 0
 * phy = 2, PHY index 1
 */
static struct io_setting *
lpddr4_get_io_settings(const struct rk3399_sdram_params *params, u32 mr5)
{
	struct io_setting *io = NULL;
	u32 n;

	for (n = 0; n < ARRAY_SIZE(lpddr4_io_setting); n++) {
		io = &lpddr4_io_setting[n];

		if (io->mr5 != 0) {
			if (io->mhz >= params->base.ddr_freq &&
			    io->mr5 == mr5)
				break;
		} else {
			if (io->mhz >= params->base.ddr_freq)
				break;
		}
	}

	return io;
}

static void *get_denali_phy(const struct chan_info *chan,
			    struct rk3399_sdram_params *params, bool reg)
{
	return reg ? &chan->publ->denali_phy : &params->phy_regs.denali_phy;
}

static void *get_denali_ctl(const struct chan_info *chan,
			    struct rk3399_sdram_params *params, bool reg)
{
	return reg ? &chan->pctl->denali_ctl : &params->pctl_regs.denali_ctl;
}

static void *get_ddrc0_con(struct dram_info *dram, u8 channel)
{
	return (channel == 0) ? &dram->grf->ddrc0_con0 : &dram->grf->ddrc0_con1;
}

static void copy_to_reg(u32 *dest, const u32 *src, u32 n)
{
	int i;

	for (i = 0; i < n / sizeof(u32); i++) {
		writel(*src, dest);
		src++;
		dest++;
	}
}

static void rkclk_ddr_reset(struct rk3399_cru *cru, u32 channel, u32 ctl,
			    u32 phy)
{
	channel &= 0x1;
	ctl &= 0x1;
	phy &= 0x1;
	writel(CRU_SFTRST_DDR_CTRL(channel, ctl) |
				   CRU_SFTRST_DDR_PHY(channel, phy),
				   &cru->softrst_con[4]);
}

static void phy_pctrl_reset(struct rk3399_cru *cru,  u32 channel)
{
	rkclk_ddr_reset(cru, channel, 1, 1);
	udelay(10);

	rkclk_ddr_reset(cru, channel, 1, 0);
	udelay(10);

	rkclk_ddr_reset(cru, channel, 0, 0);
	udelay(10);
}

static void phy_dll_bypass_set(struct rk3399_ddr_publ_regs *ddr_publ_regs,
			       u32 freq)
{
	u32 *denali_phy = ddr_publ_regs->denali_phy;

	/* From IP spec, only freq small than 125 can enter dll bypass mode */
	if (freq <= 125) {
		/* phy_sw_master_mode_X PHY_86/214/342/470 4bits offset_8 */
		setbits_le32(&denali_phy[86], (0x3 << 2) << 8);
		setbits_le32(&denali_phy[214], (0x3 << 2) << 8);
		setbits_le32(&denali_phy[342], (0x3 << 2) << 8);
		setbits_le32(&denali_phy[470], (0x3 << 2) << 8);

		/* phy_adrctl_sw_master_mode PHY_547/675/803 4bits offset_16 */
		setbits_le32(&denali_phy[547], (0x3 << 2) << 16);
		setbits_le32(&denali_phy[675], (0x3 << 2) << 16);
		setbits_le32(&denali_phy[803], (0x3 << 2) << 16);
	} else {
		/* phy_sw_master_mode_X PHY_86/214/342/470 4bits offset_8 */
		clrbits_le32(&denali_phy[86], (0x3 << 2) << 8);
		clrbits_le32(&denali_phy[214], (0x3 << 2) << 8);
		clrbits_le32(&denali_phy[342], (0x3 << 2) << 8);
		clrbits_le32(&denali_phy[470], (0x3 << 2) << 8);

		/* phy_adrctl_sw_master_mode PHY_547/675/803 4bits offset_16 */
		clrbits_le32(&denali_phy[547], (0x3 << 2) << 16);
		clrbits_le32(&denali_phy[675], (0x3 << 2) << 16);
		clrbits_le32(&denali_phy[803], (0x3 << 2) << 16);
	}
}

static void set_memory_map(const struct chan_info *chan, u32 channel,
			   const struct rk3399_sdram_params *params)
{
	const struct rk3399_sdram_channel *sdram_ch = &params->ch[channel];
	u32 *denali_ctl = chan->pctl->denali_ctl;
	u32 *denali_pi = chan->pi->denali_pi;
	u32 cs_map;
	u32 reduc;
	u32 row;

	/* Get row number from ddrconfig setting */
	if (sdram_ch->cap_info.ddrconfig < 2 ||
	    sdram_ch->cap_info.ddrconfig == 4)
		row = 16;
	else if (sdram_ch->cap_info.ddrconfig == 3)
		row = 14;
	else
		row = 15;

	cs_map = (sdram_ch->cap_info.rank > 1) ? 3 : 1;
	reduc = (sdram_ch->cap_info.bw == 2) ? 0 : 1;

	/* Set the dram configuration to ctrl */
	clrsetbits_le32(&denali_ctl[191], 0xF, (12 - sdram_ch->cap_info.col));
	clrsetbits_le32(&denali_ctl[190], (0x3 << 16) | (0x7 << 24),
			((3 - sdram_ch->cap_info.bk) << 16) |
			((16 - row) << 24));

	clrsetbits_le32(&denali_ctl[196], 0x3 | (1 << 16),
			cs_map | (reduc << 16));

	/* PI_199 PI_COL_DIFF:RW:0:4 */
	clrsetbits_le32(&denali_pi[199], 0xF, (12 - sdram_ch->cap_info.col));

	/* PI_155 PI_ROW_DIFF:RW:24:3 PI_BANK_DIFF:RW:16:2 */
	clrsetbits_le32(&denali_pi[155], (0x3 << 16) | (0x7 << 24),
			((3 - sdram_ch->cap_info.bk) << 16) |
			((16 - row) << 24));

	if (IS_ENABLED(CONFIG_RAM_RK3399_LPDDR4)) {
		if (cs_map == 1)
			cs_map = 0x5;
		else if (cs_map == 2)
			cs_map = 0xa;
		else
			cs_map = 0xF;
	}

	/* PI_41 PI_CS_MAP:RW:24:4 */
	clrsetbits_le32(&denali_pi[41], 0xf << 24, cs_map << 24);
	if (sdram_ch->cap_info.rank == 1 && params->base.dramtype == DDR3)
		writel(0x2EC7FFFF, &denali_pi[34]);
}

static int phy_io_config(const struct chan_info *chan,
			 const struct rk3399_sdram_params *params, u32 mr5)
{
	u32 *denali_phy = chan->publ->denali_phy;
	u32 vref_mode_dq, vref_value_dq, vref_mode_ac, vref_value_ac;
	u32 mode_sel;
	u32 reg_value;
	u32 drv_value, odt_value;
	u32 speed;

	/* vref setting & mode setting */
	if (params->base.dramtype == LPDDR4) {
		struct io_setting *io = lpddr4_get_io_settings(params, mr5);
		u32 rd_vref = io->rd_vref * 1000;

		if (rd_vref < 36700) {
			/* MODE_LV[2:0] = LPDDR4 (Range 2)*/
			vref_mode_dq = 0x7;
			/* MODE[2:0]= LPDDR4 Range 2(0.4*VDDQ) */
			mode_sel = 0x5;
			vref_value_dq = (rd_vref - 3300) / 521;
		} else {
			/* MODE_LV[2:0] = LPDDR4 (Range 1)*/
			vref_mode_dq = 0x6;
			/* MODE[2:0]= LPDDR4 Range 1(0.33*VDDQ) */
			mode_sel = 0x4;
			vref_value_dq = (rd_vref - 15300) / 521;
		}
		vref_mode_ac = 0x6;
		/* VDDQ/3/2=16.8% */
		vref_value_ac = 0x3;
	} else if (params->base.dramtype == LPDDR3) {
		if (params->base.odt == 1) {
			vref_mode_dq = 0x5;  /* LPDDR3 ODT */
			drv_value = (readl(&denali_phy[6]) >> 12) & 0xf;
			odt_value = (readl(&denali_phy[6]) >> 4) & 0xf;
			if (drv_value == PHY_DRV_ODT_48) {
				switch (odt_value) {
				case PHY_DRV_ODT_240:
					vref_value_dq = 0x16;
					break;
				case PHY_DRV_ODT_120:
					vref_value_dq = 0x26;
					break;
				case PHY_DRV_ODT_60:
					vref_value_dq = 0x36;
					break;
				default:
					debug("Invalid ODT value.\n");
					return -EINVAL;
				}
			} else if (drv_value == PHY_DRV_ODT_40) {
				switch (odt_value) {
				case PHY_DRV_ODT_240:
					vref_value_dq = 0x19;
					break;
				case PHY_DRV_ODT_120:
					vref_value_dq = 0x23;
					break;
				case PHY_DRV_ODT_60:
					vref_value_dq = 0x31;
					break;
				default:
					debug("Invalid ODT value.\n");
					return -EINVAL;
				}
			} else if (drv_value == PHY_DRV_ODT_34_3) {
				switch (odt_value) {
				case PHY_DRV_ODT_240:
					vref_value_dq = 0x17;
					break;
				case PHY_DRV_ODT_120:
					vref_value_dq = 0x20;
					break;
				case PHY_DRV_ODT_60:
					vref_value_dq = 0x2e;
					break;
				default:
					debug("Invalid ODT value.\n");
					return -EINVAL;
				}
			} else {
				debug("Invalid DRV value.\n");
				return -EINVAL;
			}
		} else {
			vref_mode_dq = 0x2;  /* LPDDR3 */
			vref_value_dq = 0x1f;
		}
		vref_mode_ac = 0x2;
		vref_value_ac = 0x1f;
		mode_sel = 0x0;
	} else if (params->base.dramtype == DDR3) {
		/* DDR3L */
		vref_mode_dq = 0x1;
		vref_value_dq = 0x1f;
		vref_mode_ac = 0x1;
		vref_value_ac = 0x1f;
		mode_sel = 0x1;
	} else {
		debug("Unknown DRAM type.\n");
		return -EINVAL;
	}

	reg_value = (vref_mode_dq << 9) | (0x1 << 8) | vref_value_dq;

	/* PHY_913 PHY_PAD_VREF_CTRL_DQ_0 12bits offset_8 */
	clrsetbits_le32(&denali_phy[913], 0xfff << 8, reg_value << 8);
	/* PHY_914 PHY_PAD_VREF_CTRL_DQ_1 12bits offset_0 */
	clrsetbits_le32(&denali_phy[914], 0xfff, reg_value);
	/* PHY_914 PHY_PAD_VREF_CTRL_DQ_2 12bits offset_16 */
	clrsetbits_le32(&denali_phy[914], 0xfff << 16, reg_value << 16);
	/* PHY_915 PHY_PAD_VREF_CTRL_DQ_3 12bits offset_0 */
	clrsetbits_le32(&denali_phy[915], 0xfff, reg_value);

	reg_value = (vref_mode_ac << 9) | (0x1 << 8) | vref_value_ac;

	/* PHY_915 PHY_PAD_VREF_CTRL_AC 12bits offset_16 */
	clrsetbits_le32(&denali_phy[915], 0xfff << 16, reg_value << 16);

	/* PHY_924 PHY_PAD_FDBK_DRIVE */
	clrsetbits_le32(&denali_phy[924], 0x7 << 15, mode_sel << 15);
	/* PHY_926 PHY_PAD_DATA_DRIVE */
	clrsetbits_le32(&denali_phy[926], 0x7 << 6, mode_sel << 6);
	/* PHY_927 PHY_PAD_DQS_DRIVE */
	clrsetbits_le32(&denali_phy[927], 0x7 << 6, mode_sel << 6);
	/* PHY_928 PHY_PAD_ADDR_DRIVE */
	clrsetbits_le32(&denali_phy[928], 0x7 << 14, mode_sel << 14);
	/* PHY_929 PHY_PAD_CLK_DRIVE */
	clrsetbits_le32(&denali_phy[929], 0x7 << 14, mode_sel << 14);
	/* PHY_935 PHY_PAD_CKE_DRIVE */
	clrsetbits_le32(&denali_phy[935], 0x7 << 14, mode_sel << 14);
	/* PHY_937 PHY_PAD_RST_DRIVE */
	clrsetbits_le32(&denali_phy[937], 0x7 << 14, mode_sel << 14);
	/* PHY_939 PHY_PAD_CS_DRIVE */
	clrsetbits_le32(&denali_phy[939], 0x7 << 14, mode_sel << 14);

	if (IS_ENABLED(CONFIG_RAM_RK3399_LPDDR4)) {
		/* BOOSTP_EN & BOOSTN_EN */
		reg_value = ((PHY_BOOSTP_EN << 4) | PHY_BOOSTN_EN);
		/* PHY_925 PHY_PAD_FDBK_DRIVE2 */
		clrsetbits_le32(&denali_phy[925], 0xff << 8, reg_value << 8);
		/* PHY_926 PHY_PAD_DATA_DRIVE */
		clrsetbits_le32(&denali_phy[926], 0xff << 12, reg_value << 12);
		/* PHY_927 PHY_PAD_DQS_DRIVE */
		clrsetbits_le32(&denali_phy[927], 0xff << 14, reg_value << 14);
		/* PHY_928 PHY_PAD_ADDR_DRIVE */
		clrsetbits_le32(&denali_phy[928], 0xff << 20, reg_value << 20);
		/* PHY_929 PHY_PAD_CLK_DRIVE */
		clrsetbits_le32(&denali_phy[929], 0xff << 22, reg_value << 22);
		/* PHY_935 PHY_PAD_CKE_DRIVE */
		clrsetbits_le32(&denali_phy[935], 0xff << 20, reg_value << 20);
		/* PHY_937 PHY_PAD_RST_DRIVE */
		clrsetbits_le32(&denali_phy[937], 0xff << 20, reg_value << 20);
		/* PHY_939 PHY_PAD_CS_DRIVE */
		clrsetbits_le32(&denali_phy[939], 0xff << 20, reg_value << 20);

		/* SLEWP_EN & SLEWN_EN */
		reg_value = ((PHY_SLEWP_EN << 3) | PHY_SLEWN_EN);
		/* PHY_924 PHY_PAD_FDBK_DRIVE */
		clrsetbits_le32(&denali_phy[924], 0x3f << 8, reg_value << 8);
		/* PHY_926 PHY_PAD_DATA_DRIVE */
		clrsetbits_le32(&denali_phy[926], 0x3f, reg_value);
		/* PHY_927 PHY_PAD_DQS_DRIVE */
		clrsetbits_le32(&denali_phy[927], 0x3f, reg_value);
		/* PHY_928 PHY_PAD_ADDR_DRIVE */
		clrsetbits_le32(&denali_phy[928], 0x3f << 8, reg_value << 8);
		/* PHY_929 PHY_PAD_CLK_DRIVE */
		clrsetbits_le32(&denali_phy[929], 0x3f << 8, reg_value << 8);
		/* PHY_935 PHY_PAD_CKE_DRIVE */
		clrsetbits_le32(&denali_phy[935], 0x3f << 8, reg_value << 8);
		/* PHY_937 PHY_PAD_RST_DRIVE */
		clrsetbits_le32(&denali_phy[937], 0x3f << 8, reg_value << 8);
		/* PHY_939 PHY_PAD_CS_DRIVE */
		clrsetbits_le32(&denali_phy[939], 0x3f << 8, reg_value << 8);
	}

	/* speed setting */
	if (params->base.ddr_freq < 400)
		speed = 0x0;
	else if (params->base.ddr_freq < 800)
		speed = 0x1;
	else if (params->base.ddr_freq < 1200)
		speed = 0x2;
	else
		speed = 0x3;

	/* PHY_924 PHY_PAD_FDBK_DRIVE */
	clrsetbits_le32(&denali_phy[924], 0x3 << 21, speed << 21);
	/* PHY_926 PHY_PAD_DATA_DRIVE */
	clrsetbits_le32(&denali_phy[926], 0x3 << 9, speed << 9);
	/* PHY_927 PHY_PAD_DQS_DRIVE */
	clrsetbits_le32(&denali_phy[927], 0x3 << 9, speed << 9);
	/* PHY_928 PHY_PAD_ADDR_DRIVE */
	clrsetbits_le32(&denali_phy[928], 0x3 << 17, speed << 17);
	/* PHY_929 PHY_PAD_CLK_DRIVE */
	clrsetbits_le32(&denali_phy[929], 0x3 << 17, speed << 17);
	/* PHY_935 PHY_PAD_CKE_DRIVE */
	clrsetbits_le32(&denali_phy[935], 0x3 << 17, speed << 17);
	/* PHY_937 PHY_PAD_RST_DRIVE */
	clrsetbits_le32(&denali_phy[937], 0x3 << 17, speed << 17);
	/* PHY_939 PHY_PAD_CS_DRIVE */
	clrsetbits_le32(&denali_phy[939], 0x3 << 17, speed << 17);

	if (IS_ENABLED(CONFIG_RAM_RK3399_LPDDR4)) {
		/* RX_CM_INPUT */
		reg_value = PHY_RX_CM_INPUT;
		/* PHY_924 PHY_PAD_FDBK_DRIVE */
		clrsetbits_le32(&denali_phy[924], 0x1 << 14, reg_value << 14);
		/* PHY_926 PHY_PAD_DATA_DRIVE */
		clrsetbits_le32(&denali_phy[926], 0x1 << 11, reg_value << 11);
		/* PHY_927 PHY_PAD_DQS_DRIVE */
		clrsetbits_le32(&denali_phy[927], 0x1 << 13, reg_value << 13);
		/* PHY_928 PHY_PAD_ADDR_DRIVE */
		clrsetbits_le32(&denali_phy[928], 0x1 << 19, reg_value << 19);
		/* PHY_929 PHY_PAD_CLK_DRIVE */
		clrsetbits_le32(&denali_phy[929], 0x1 << 21, reg_value << 21);
		/* PHY_935 PHY_PAD_CKE_DRIVE */
		clrsetbits_le32(&denali_phy[935], 0x1 << 19, reg_value << 19);
		/* PHY_937 PHY_PAD_RST_DRIVE */
		clrsetbits_le32(&denali_phy[937], 0x1 << 19, reg_value << 19);
		/* PHY_939 PHY_PAD_CS_DRIVE */
		clrsetbits_le32(&denali_phy[939], 0x1 << 19, reg_value << 19);
	}

	return 0;
}

static void set_ds_odt(const struct chan_info *chan,
		       struct rk3399_sdram_params *params,
		       bool ctl_phy_reg, u32 mr5)
{
	u32 *denali_phy = get_denali_phy(chan, params, ctl_phy_reg);
	u32 *denali_ctl = get_denali_ctl(chan, params, ctl_phy_reg);
	u32 tsel_idle_en, tsel_wr_en, tsel_rd_en;
	u32 tsel_idle_select_p, tsel_rd_select_p;
	u32 tsel_idle_select_n, tsel_rd_select_n;
	u32 tsel_wr_select_dq_p, tsel_wr_select_ca_p;
	u32 tsel_wr_select_dq_n, tsel_wr_select_ca_n;
	u32 tsel_ckcs_select_p, tsel_ckcs_select_n;
	struct io_setting *io = NULL;
	u32 soc_odt = 0;
	u32 reg_value;

	if (params->base.dramtype == LPDDR4) {
		io = lpddr4_get_io_settings(params, mr5);

		tsel_rd_select_p = PHY_DRV_ODT_HI_Z;
		tsel_rd_select_n = io->rd_odt;

		tsel_idle_select_p = PHY_DRV_ODT_HI_Z;
		tsel_idle_select_n = PHY_DRV_ODT_240;

		tsel_wr_select_dq_p = io->wr_dq_drv;
		tsel_wr_select_dq_n = PHY_DRV_ODT_40;

		tsel_wr_select_ca_p = io->wr_ca_drv;
		tsel_wr_select_ca_n = PHY_DRV_ODT_40;

		tsel_ckcs_select_p = io->wr_ckcs_drv;
		tsel_ckcs_select_n = PHY_DRV_ODT_34_3;
		switch (tsel_rd_select_n) {
		case PHY_DRV_ODT_240:
			soc_odt = 1;
			break;
		case PHY_DRV_ODT_120:
			soc_odt = 2;
			break;
		case PHY_DRV_ODT_80:
			soc_odt = 3;
			break;
		case PHY_DRV_ODT_60:
			soc_odt = 4;
			break;
		case PHY_DRV_ODT_48:
			soc_odt = 5;
			break;
		case PHY_DRV_ODT_40:
			soc_odt = 6;
			break;
		case PHY_DRV_ODT_34_3:
			soc_odt = 6;
			printf("%s: Unable to support LPDDR4 MR22 Soc ODT\n",
			       __func__);
			break;
		case PHY_DRV_ODT_HI_Z:
		default:
			soc_odt = 0;
			break;
		}
	} else if (params->base.dramtype == LPDDR3) {
		tsel_rd_select_p = PHY_DRV_ODT_240;
		tsel_rd_select_n = PHY_DRV_ODT_HI_Z;

		tsel_idle_select_p = PHY_DRV_ODT_240;
		tsel_idle_select_n = PHY_DRV_ODT_HI_Z;

		tsel_wr_select_dq_p = PHY_DRV_ODT_34_3;
		tsel_wr_select_dq_n = PHY_DRV_ODT_34_3;

		tsel_wr_select_ca_p = PHY_DRV_ODT_48;
		tsel_wr_select_ca_n = PHY_DRV_ODT_48;

		tsel_ckcs_select_p = PHY_DRV_ODT_34_3;
		tsel_ckcs_select_n = PHY_DRV_ODT_34_3;
	} else {
		tsel_rd_select_p = PHY_DRV_ODT_240;
		tsel_rd_select_n = PHY_DRV_ODT_240;

		tsel_idle_select_p = PHY_DRV_ODT_240;
		tsel_idle_select_n = PHY_DRV_ODT_240;

		tsel_wr_select_dq_p = PHY_DRV_ODT_34_3;
		tsel_wr_select_dq_n = PHY_DRV_ODT_34_3;

		tsel_wr_select_ca_p = PHY_DRV_ODT_34_3;
		tsel_wr_select_ca_n = PHY_DRV_ODT_34_3;

		tsel_ckcs_select_p = PHY_DRV_ODT_34_3;
		tsel_ckcs_select_n = PHY_DRV_ODT_34_3;
	}

	if (params->base.odt == 1) {
		tsel_rd_en = 1;

		if (params->base.dramtype == LPDDR4)
			tsel_rd_en = io->rd_odt_en;
	} else {
		tsel_rd_en = 0;
	}

	tsel_wr_en = 0;
	tsel_idle_en = 0;

	/* F0_0 */
	clrsetbits_le32(&denali_ctl[145], 0xFF << 16,
			(soc_odt | (CS0_MR22_VAL << 3)) << 16);
	/* F2_0, F1_0 */
	clrsetbits_le32(&denali_ctl[146], 0xFF00FF,
			((soc_odt | (CS0_MR22_VAL << 3)) << 16) |
			(soc_odt | (CS0_MR22_VAL << 3)));
	/* F0_1 */
	clrsetbits_le32(&denali_ctl[159], 0xFF << 16,
			(soc_odt | (CS1_MR22_VAL << 3)) << 16);
	/* F2_1, F1_1 */
	clrsetbits_le32(&denali_ctl[160], 0xFF00FF,
			((soc_odt | (CS1_MR22_VAL << 3)) << 16) |
			(soc_odt | (CS1_MR22_VAL << 3)));

	/*
	 * phy_dq_tsel_select_X 24bits DENALI_PHY_6/134/262/390 offset_0
	 * sets termination values for read/idle cycles and drive strength
	 * for write cycles for DQ/DM
	 */
	reg_value = tsel_rd_select_n | (tsel_rd_select_p << 0x4) |
		    (tsel_wr_select_dq_n << 8) | (tsel_wr_select_dq_p << 12) |
		    (tsel_idle_select_n << 16) | (tsel_idle_select_p << 20);
	clrsetbits_le32(&denali_phy[6], 0xffffff, reg_value);
	clrsetbits_le32(&denali_phy[134], 0xffffff, reg_value);
	clrsetbits_le32(&denali_phy[262], 0xffffff, reg_value);
	clrsetbits_le32(&denali_phy[390], 0xffffff, reg_value);

	/*
	 * phy_dqs_tsel_select_X 24bits DENALI_PHY_7/135/263/391 offset_0
	 * sets termination values for read/idle cycles and drive strength
	 * for write cycles for DQS
	 */
	clrsetbits_le32(&denali_phy[7], 0xffffff, reg_value);
	clrsetbits_le32(&denali_phy[135], 0xffffff, reg_value);
	clrsetbits_le32(&denali_phy[263], 0xffffff, reg_value);
	clrsetbits_le32(&denali_phy[391], 0xffffff, reg_value);

	/* phy_adr_tsel_select_ 8bits DENALI_PHY_544/672/800 offset_0 */
	reg_value = tsel_wr_select_ca_n | (tsel_wr_select_ca_p << 0x4);
	if (IS_ENABLED(CONFIG_RAM_RK3399_LPDDR4)) {
		/* LPDDR4 these register read always return 0, so
		 * can not use clrsetbits_le32(), need to write32
		 */
		writel((0x300 << 8) | reg_value, &denali_phy[544]);
		writel((0x300 << 8) | reg_value, &denali_phy[672]);
		writel((0x300 << 8) | reg_value, &denali_phy[800]);
	} else {
		clrsetbits_le32(&denali_phy[544], 0xff, reg_value);
		clrsetbits_le32(&denali_phy[672], 0xff, reg_value);
		clrsetbits_le32(&denali_phy[800], 0xff, reg_value);
	}

	/* phy_pad_addr_drive 8bits DENALI_PHY_928 offset_0 */
	clrsetbits_le32(&denali_phy[928], 0xff, reg_value);

	/* phy_pad_rst_drive 8bits DENALI_PHY_937 offset_0 */
	if (!ctl_phy_reg)
		clrsetbits_le32(&denali_phy[937], 0xff, reg_value);

	/* phy_pad_cke_drive 8bits DENALI_PHY_935 offset_0 */
	clrsetbits_le32(&denali_phy[935], 0xff, reg_value);

	/* phy_pad_cs_drive 8bits DENALI_PHY_939 offset_0 */
	clrsetbits_le32(&denali_phy[939], 0xff,
			tsel_ckcs_select_n | (tsel_ckcs_select_p << 0x4));

	/* phy_pad_clk_drive 8bits DENALI_PHY_929 offset_0 */
	clrsetbits_le32(&denali_phy[929], 0xff,
			tsel_ckcs_select_n | (tsel_ckcs_select_p << 0x4));

	/* phy_pad_fdbk_drive 23bit DENALI_PHY_924/925 */
	clrsetbits_le32(&denali_phy[924], 0xff,
			tsel_wr_select_dq_n | (tsel_wr_select_dq_p << 4));
	clrsetbits_le32(&denali_phy[925], 0xff,
			tsel_rd_select_n | (tsel_rd_select_p << 4));

	/* phy_dq_tsel_enable_X 3bits DENALI_PHY_5/133/261/389 offset_16 */
	reg_value = (tsel_rd_en | (tsel_wr_en << 1) | (tsel_idle_en << 2))
		<< 16;
	clrsetbits_le32(&denali_phy[5], 0x7 << 16, reg_value);
	clrsetbits_le32(&denali_phy[133], 0x7 << 16, reg_value);
	clrsetbits_le32(&denali_phy[261], 0x7 << 16, reg_value);
	clrsetbits_le32(&denali_phy[389], 0x7 << 16, reg_value);

	/* phy_dqs_tsel_enable_X 3bits DENALI_PHY_6/134/262/390 offset_24 */
	reg_value = (tsel_rd_en | (tsel_wr_en << 1) | (tsel_idle_en << 2))
		<< 24;
	clrsetbits_le32(&denali_phy[6], 0x7 << 24, reg_value);
	clrsetbits_le32(&denali_phy[134], 0x7 << 24, reg_value);
	clrsetbits_le32(&denali_phy[262], 0x7 << 24, reg_value);
	clrsetbits_le32(&denali_phy[390], 0x7 << 24, reg_value);

	/* phy_adr_tsel_enable_ 1bit DENALI_PHY_518/646/774 offset_8 */
	reg_value = tsel_wr_en << 8;
	clrsetbits_le32(&denali_phy[518], 0x1 << 8, reg_value);
	clrsetbits_le32(&denali_phy[646], 0x1 << 8, reg_value);
	clrsetbits_le32(&denali_phy[774], 0x1 << 8, reg_value);

	/* phy_pad_addr_term tsel 1bit DENALI_PHY_933 offset_17 */
	reg_value = tsel_wr_en << 17;
	clrsetbits_le32(&denali_phy[933], 0x1 << 17, reg_value);
	/*
	 * pad_rst/cke/cs/clk_term tsel 1bits
	 * DENALI_PHY_938/936/940/934 offset_17
	 */
	clrsetbits_le32(&denali_phy[938], 0x1 << 17, reg_value);
	clrsetbits_le32(&denali_phy[936], 0x1 << 17, reg_value);
	clrsetbits_le32(&denali_phy[940], 0x1 << 17, reg_value);
	clrsetbits_le32(&denali_phy[934], 0x1 << 17, reg_value);

	/* phy_pad_fdbk_term 1bit DENALI_PHY_930 offset_17 */
	clrsetbits_le32(&denali_phy[930], 0x1 << 17, reg_value);

	phy_io_config(chan, params, mr5);
}

static void pctl_start(struct dram_info *dram, u8 channel)
{
	const struct chan_info *chan = &dram->chan[channel];
	u32 *denali_ctl = chan->pctl->denali_ctl;
	u32 *denali_phy = chan->publ->denali_phy;
	u32 *ddrc0_con = get_ddrc0_con(dram, channel);
	u32 count = 0;
	u32 byte, tmp;

	writel(0x01000000, &ddrc0_con);

	clrsetbits_le32(&denali_phy[957], 0x3 << 24, 0x2 << 24);

	while (!(readl(&denali_ctl[203]) & (1 << 3))) {
		if (count > 1000) {
			printf("%s: Failed to init pctl for channel %d\n",
			       __func__, channel);
			while (1)
				;
		}

		udelay(1);
		count++;
	}

	writel(0x01000100, &ddrc0_con);

	for (byte = 0; byte < 4; byte++) {
		tmp = 0x820;
		writel((tmp << 16) | tmp, &denali_phy[53 + (128 * byte)]);
		writel((tmp << 16) | tmp, &denali_phy[54 + (128 * byte)]);
		writel((tmp << 16) | tmp, &denali_phy[55 + (128 * byte)]);
		writel((tmp << 16) | tmp, &denali_phy[56 + (128 * byte)]);
		writel((tmp << 16) | tmp, &denali_phy[57 + (128 * byte)]);

		clrsetbits_le32(&denali_phy[58 + (128 * byte)], 0xffff, tmp);
	}

	clrsetbits_le32(&denali_ctl[68], PWRUP_SREFRESH_EXIT,
			dram->pwrup_srefresh_exit[channel]);
}

static int pctl_cfg(struct dram_info *dram, const struct chan_info *chan,
		    u32 channel, struct rk3399_sdram_params *params)
{
	u32 *denali_ctl = chan->pctl->denali_ctl;
	u32 *denali_pi = chan->pi->denali_pi;
	u32 *denali_phy = chan->publ->denali_phy;
	const u32 *params_ctl = params->pctl_regs.denali_ctl;
	const u32 *params_phy = params->phy_regs.denali_phy;
	u32 tmp, tmp1, tmp2;

	/*
	 * work around controller bug:
	 * Do not program DRAM_CLASS until NO_PHY_IND_TRAIN_INT is programmed
	 */
	copy_to_reg(&denali_ctl[1], &params_ctl[1],
		    sizeof(struct rk3399_ddr_pctl_regs) - 4);
	writel(params_ctl[0], &denali_ctl[0]);

	/*
	 * two channel init at the same time, then ZQ Cal Start
	 * at the same time, it will use the same RZQ, but cannot
	 * start at the same time.
	 *
	 * So, increase tINIT3 for channel 1, will avoid two
	 * channel ZQ Cal Start at the same time
	 */
	if (params->base.dramtype == LPDDR4 && channel == 1) {
		tmp = ((params->base.ddr_freq * MHz + 999) / 1000);
		tmp1 = readl(&denali_ctl[14]);
		writel(tmp + tmp1, &denali_ctl[14]);
	}

	copy_to_reg(denali_pi, &params->pi_regs.denali_pi[0],
		    sizeof(struct rk3399_ddr_pi_regs));

	/* rank count need to set for init */
	set_memory_map(chan, channel, params);

	writel(params->phy_regs.denali_phy[910], &denali_phy[910]);
	writel(params->phy_regs.denali_phy[911], &denali_phy[911]);
	writel(params->phy_regs.denali_phy[912], &denali_phy[912]);

	if (IS_ENABLED(CONFIG_RAM_RK3399_LPDDR4)) {
		writel(params->phy_regs.denali_phy[898], &denali_phy[898]);
		writel(params->phy_regs.denali_phy[919], &denali_phy[919]);
	}

	dram->pwrup_srefresh_exit[channel] = readl(&denali_ctl[68]) &
					     PWRUP_SREFRESH_EXIT;
	clrbits_le32(&denali_ctl[68], PWRUP_SREFRESH_EXIT);

	/* PHY_DLL_RST_EN */
	clrsetbits_le32(&denali_phy[957], 0x3 << 24, 1 << 24);

	setbits_le32(&denali_pi[0], START);
	setbits_le32(&denali_ctl[0], START);

	/**
	 * LPDDR4 use PLL bypass mode for init
	 * not need to wait for the PLL to lock
	 */
	if (params->base.dramtype != LPDDR4) {
		/* Waiting for phy DLL lock */
		while (1) {
			tmp = readl(&denali_phy[920]);
			tmp1 = readl(&denali_phy[921]);
			tmp2 = readl(&denali_phy[922]);
			if ((((tmp >> 16) & 0x1) == 0x1) &&
			    (((tmp1 >> 16) & 0x1) == 0x1) &&
			    (((tmp1 >> 0) & 0x1) == 0x1) &&
			    (((tmp2 >> 0) & 0x1) == 0x1))
				break;
		}
	}

	copy_to_reg(&denali_phy[896], &params_phy[896], (958 - 895) * 4);
	copy_to_reg(&denali_phy[0], &params_phy[0], (90 - 0 + 1) * 4);
	copy_to_reg(&denali_phy[128], &params_phy[128], (218 - 128 + 1) * 4);
	copy_to_reg(&denali_phy[256], &params_phy[256], (346 - 256 + 1) * 4);
	copy_to_reg(&denali_phy[384], &params_phy[384], (474 - 384 + 1) * 4);
	copy_to_reg(&denali_phy[512], &params_phy[512], (549 - 512 + 1) * 4);
	copy_to_reg(&denali_phy[640], &params_phy[640], (677 - 640 + 1) * 4);
	copy_to_reg(&denali_phy[768], &params_phy[768], (805 - 768 + 1) * 4);
	set_ds_odt(chan, params, true, 0);

	/*
	 * phy_dqs_tsel_wr_timing_X 8bits DENALI_PHY_84/212/340/468 offset_8
	 * dqs_tsel_wr_end[7:4] add Half cycle
	 */
	tmp = (readl(&denali_phy[84]) >> 8) & 0xff;
	clrsetbits_le32(&denali_phy[84], 0xff << 8, (tmp + 0x10) << 8);
	tmp = (readl(&denali_phy[212]) >> 8) & 0xff;
	clrsetbits_le32(&denali_phy[212], 0xff << 8, (tmp + 0x10) << 8);
	tmp = (readl(&denali_phy[340]) >> 8) & 0xff;
	clrsetbits_le32(&denali_phy[340], 0xff << 8, (tmp + 0x10) << 8);
	tmp = (readl(&denali_phy[468]) >> 8) & 0xff;
	clrsetbits_le32(&denali_phy[468], 0xff << 8, (tmp + 0x10) << 8);

	/*
	 * phy_dqs_tsel_wr_timing_X 8bits DENALI_PHY_83/211/339/467 offset_8
	 * dq_tsel_wr_end[7:4] add Half cycle
	 */
	tmp = (readl(&denali_phy[83]) >> 16) & 0xff;
	clrsetbits_le32(&denali_phy[83], 0xff << 16, (tmp + 0x10) << 16);
	tmp = (readl(&denali_phy[211]) >> 16) & 0xff;
	clrsetbits_le32(&denali_phy[211], 0xff << 16, (tmp + 0x10) << 16);
	tmp = (readl(&denali_phy[339]) >> 16) & 0xff;
	clrsetbits_le32(&denali_phy[339], 0xff << 16, (tmp + 0x10) << 16);
	tmp = (readl(&denali_phy[467]) >> 16) & 0xff;
	clrsetbits_le32(&denali_phy[467], 0xff << 16, (tmp + 0x10) << 16);

	return 0;
}

static void select_per_cs_training_index(const struct chan_info *chan,
					 u32 rank)
{
	u32 *denali_phy = chan->publ->denali_phy;

	/* PHY_84 PHY_PER_CS_TRAINING_EN_0 1bit offset_16 */
	if ((readl(&denali_phy[84]) >> 16) & 1) {
		/*
		 * PHY_8/136/264/392
		 * phy_per_cs_training_index_X 1bit offset_24
		 */
		clrsetbits_le32(&denali_phy[8], 0x1 << 24, rank << 24);
		clrsetbits_le32(&denali_phy[136], 0x1 << 24, rank << 24);
		clrsetbits_le32(&denali_phy[264], 0x1 << 24, rank << 24);
		clrsetbits_le32(&denali_phy[392], 0x1 << 24, rank << 24);
	}
}

static void override_write_leveling_value(const struct chan_info *chan)
{
	u32 *denali_ctl = chan->pctl->denali_ctl;
	u32 *denali_phy = chan->publ->denali_phy;
	u32 byte;

	/* PHY_896 PHY_FREQ_SEL_MULTICAST_EN 1bit offset_0 */
	setbits_le32(&denali_phy[896], 1);

	/*
	 * PHY_8/136/264/392
	 * phy_per_cs_training_multicast_en_X 1bit offset_16
	 */
	clrsetbits_le32(&denali_phy[8], 0x1 << 16, 1 << 16);
	clrsetbits_le32(&denali_phy[136], 0x1 << 16, 1 << 16);
	clrsetbits_le32(&denali_phy[264], 0x1 << 16, 1 << 16);
	clrsetbits_le32(&denali_phy[392], 0x1 << 16, 1 << 16);

	for (byte = 0; byte < 4; byte++)
		clrsetbits_le32(&denali_phy[63 + (128 * byte)], 0xffff << 16,
				0x200 << 16);

	/* PHY_896 PHY_FREQ_SEL_MULTICAST_EN 1bit offset_0 */
	clrbits_le32(&denali_phy[896], 1);

	/* CTL_200 ctrlupd_req 1bit offset_8 */
	clrsetbits_le32(&denali_ctl[200], 0x1 << 8, 0x1 << 8);
}

static int data_training_ca(const struct chan_info *chan, u32 channel,
			    const struct rk3399_sdram_params *params)
{
	u32 *denali_pi = chan->pi->denali_pi;
	u32 *denali_phy = chan->publ->denali_phy;
	u32 i, tmp;
	u32 obs_0, obs_1, obs_2, obs_err = 0;
	u32 rank = params->ch[channel].cap_info.rank;
	u32 rank_mask;

	/* clear interrupt,PI_175 PI_INT_ACK:WR:0:17 */
	writel(0x00003f7c, (&denali_pi[175]));

	if (params->base.dramtype == LPDDR4)
		rank_mask = (rank == 1) ? 0x5 : 0xf;
	else
		rank_mask = (rank == 1) ? 0x1 : 0x3;

	for (i = 0; i < 4; i++) {
		if (!(rank_mask & (1 << i)))
			continue;

		select_per_cs_training_index(chan, i);

		/* PI_100 PI_CALVL_EN:RW:8:2 */
		clrsetbits_le32(&denali_pi[100], 0x3 << 8, 0x2 << 8);

		/* PI_92 PI_CALVL_REQ:WR:16:1,PI_CALVL_CS:RW:24:2 */
		clrsetbits_le32(&denali_pi[92],
				(0x1 << 16) | (0x3 << 24),
				(0x1 << 16) | (i << 24));

		/* Waiting for training complete */
		while (1) {
			/* PI_174 PI_INT_STATUS:RD:8:18 */
			tmp = readl(&denali_pi[174]) >> 8;
			/*
			 * check status obs
			 * PHY_532/660/789 phy_adr_calvl_obs1_:0:32
			 */
			obs_0 = readl(&denali_phy[532]);
			obs_1 = readl(&denali_phy[660]);
			obs_2 = readl(&denali_phy[788]);
			if (((obs_0 >> 30) & 0x3) ||
			    ((obs_1 >> 30) & 0x3) ||
			    ((obs_2 >> 30) & 0x3))
				obs_err = 1;
			if ((((tmp >> 11) & 0x1) == 0x1) &&
			    (((tmp >> 13) & 0x1) == 0x1) &&
			    (((tmp >> 5) & 0x1) == 0x0) &&
			    obs_err == 0)
				break;
			else if ((((tmp >> 5) & 0x1) == 0x1) ||
				 (obs_err == 1))
				return -EIO;
		}

		/* clear interrupt,PI_175 PI_INT_ACK:WR:0:17 */
		writel(0x00003f7c, (&denali_pi[175]));
	}

	clrbits_le32(&denali_pi[100], 0x3 << 8);

	return 0;
}

static int data_training_wl(const struct chan_info *chan, u32 channel,
			    const struct rk3399_sdram_params *params)
{
	u32 *denali_pi = chan->pi->denali_pi;
	u32 *denali_phy = chan->publ->denali_phy;
	u32 i, tmp;
	u32 obs_0, obs_1, obs_2, obs_3, obs_err = 0;
	u32 rank = params->ch[channel].cap_info.rank;

	/* clear interrupt,PI_175 PI_INT_ACK:WR:0:17 */
	writel(0x00003f7c, (&denali_pi[175]));

	for (i = 0; i < rank; i++) {
		select_per_cs_training_index(chan, i);

		/* PI_60 PI_WRLVL_EN:RW:8:2 */
		clrsetbits_le32(&denali_pi[60], 0x3 << 8, 0x2 << 8);

		/* PI_59 PI_WRLVL_REQ:WR:8:1,PI_WRLVL_CS:RW:16:2 */
		clrsetbits_le32(&denali_pi[59],
				(0x1 << 8) | (0x3 << 16),
				(0x1 << 8) | (i << 16));

		/* Waiting for training complete */
		while (1) {
			/* PI_174 PI_INT_STATUS:RD:8:18 */
			tmp = readl(&denali_pi[174]) >> 8;

			/*
			 * check status obs, if error maybe can not
			 * get leveling done PHY_40/168/296/424
			 * phy_wrlvl_status_obs_X:0:13
			 */
			obs_0 = readl(&denali_phy[40]);
			obs_1 = readl(&denali_phy[168]);
			obs_2 = readl(&denali_phy[296]);
			obs_3 = readl(&denali_phy[424]);
			if (((obs_0 >> 12) & 0x1) ||
			    ((obs_1 >> 12) & 0x1) ||
			    ((obs_2 >> 12) & 0x1) ||
			    ((obs_3 >> 12) & 0x1))
				obs_err = 1;
			if ((((tmp >> 10) & 0x1) == 0x1) &&
			    (((tmp >> 13) & 0x1) == 0x1) &&
			    (((tmp >> 4) & 0x1) == 0x0) &&
			    obs_err == 0)
				break;
			else if ((((tmp >> 4) & 0x1) == 0x1) ||
				 (obs_err == 1))
				return -EIO;
		}

		/* clear interrupt,PI_175 PI_INT_ACK:WR:0:17 */
		writel(0x00003f7c, (&denali_pi[175]));
	}

	override_write_leveling_value(chan);
	clrbits_le32(&denali_pi[60], 0x3 << 8);

	return 0;
}

static int data_training_rg(const struct chan_info *chan, u32 channel,
			    const struct rk3399_sdram_params *params)
{
	u32 *denali_pi = chan->pi->denali_pi;
	u32 *denali_phy = chan->publ->denali_phy;
	u32 i, tmp;
	u32 obs_0, obs_1, obs_2, obs_3, obs_err = 0;
	u32 rank = params->ch[channel].cap_info.rank;

	/* clear interrupt,PI_175 PI_INT_ACK:WR:0:17 */
	writel(0x00003f7c, (&denali_pi[175]));

	for (i = 0; i < rank; i++) {
		select_per_cs_training_index(chan, i);

		/* PI_80 PI_RDLVL_GATE_EN:RW:24:2 */
		clrsetbits_le32(&denali_pi[80], 0x3 << 24, 0x2 << 24);

		/*
		 * PI_74 PI_RDLVL_GATE_REQ:WR:16:1
		 * PI_RDLVL_CS:RW:24:2
		 */
		clrsetbits_le32(&denali_pi[74],
				(0x1 << 16) | (0x3 << 24),
				(0x1 << 16) | (i << 24));

		/* Waiting for training complete */
		while (1) {
			/* PI_174 PI_INT_STATUS:RD:8:18 */
			tmp = readl(&denali_pi[174]) >> 8;

			/*
			 * check status obs
			 * PHY_43/171/299/427
			 *     PHY_GTLVL_STATUS_OBS_x:16:8
			 */
			obs_0 = readl(&denali_phy[43]);
			obs_1 = readl(&denali_phy[171]);
			obs_2 = readl(&denali_phy[299]);
			obs_3 = readl(&denali_phy[427]);
			if (((obs_0 >> (16 + 6)) & 0x3) ||
			    ((obs_1 >> (16 + 6)) & 0x3) ||
			    ((obs_2 >> (16 + 6)) & 0x3) ||
			    ((obs_3 >> (16 + 6)) & 0x3))
				obs_err = 1;
			if ((((tmp >> 9) & 0x1) == 0x1) &&
			    (((tmp >> 13) & 0x1) == 0x1) &&
			    (((tmp >> 3) & 0x1) == 0x0) &&
			    obs_err == 0)
				break;
			else if ((((tmp >> 3) & 0x1) == 0x1) ||
				 (obs_err == 1))
				return -EIO;
		}

		/* clear interrupt,PI_175 PI_INT_ACK:WR:0:17 */
		writel(0x00003f7c, (&denali_pi[175]));
	}

	clrbits_le32(&denali_pi[80], 0x3 << 24);

	return 0;
}

static int data_training_rl(const struct chan_info *chan, u32 channel,
			    const struct rk3399_sdram_params *params)
{
	u32 *denali_pi = chan->pi->denali_pi;
	u32 i, tmp;
	u32 rank = params->ch[channel].cap_info.rank;

	/* clear interrupt,PI_175 PI_INT_ACK:WR:0:17 */
	writel(0x00003f7c, (&denali_pi[175]));

	for (i = 0; i < rank; i++) {
		select_per_cs_training_index(chan, i);

		/* PI_80 PI_RDLVL_EN:RW:16:2 */
		clrsetbits_le32(&denali_pi[80], 0x3 << 16, 0x2 << 16);

		/* PI_74 PI_RDLVL_REQ:WR:8:1,PI_RDLVL_CS:RW:24:2 */
		clrsetbits_le32(&denali_pi[74],
				(0x1 << 8) | (0x3 << 24),
				(0x1 << 8) | (i << 24));

		/* Waiting for training complete */
		while (1) {
			/* PI_174 PI_INT_STATUS:RD:8:18 */
			tmp = readl(&denali_pi[174]) >> 8;

			/*
			 * make sure status obs not report error bit
			 * PHY_46/174/302/430
			 *     phy_rdlvl_status_obs_X:16:8
			 */
			if ((((tmp >> 8) & 0x1) == 0x1) &&
			    (((tmp >> 13) & 0x1) == 0x1) &&
			    (((tmp >> 2) & 0x1) == 0x0))
				break;
			else if (((tmp >> 2) & 0x1) == 0x1)
				return -EIO;
		}

		/* clear interrupt,PI_175 PI_INT_ACK:WR:0:17 */
		writel(0x00003f7c, (&denali_pi[175]));
	}

	clrbits_le32(&denali_pi[80], 0x3 << 16);

	return 0;
}

static int data_training_wdql(const struct chan_info *chan, u32 channel,
			      const struct rk3399_sdram_params *params)
{
	u32 *denali_pi = chan->pi->denali_pi;
	u32 i, tmp;
	u32 rank = params->ch[channel].cap_info.rank;
	u32 rank_mask;

	/* clear interrupt,PI_175 PI_INT_ACK:WR:0:17 */
	writel(0x00003f7c, (&denali_pi[175]));

	if (params->base.dramtype == LPDDR4)
		rank_mask = (rank == 1) ? 0x5 : 0xf;
	else
		rank_mask = (rank == 1) ? 0x1 : 0x3;

	for (i = 0; i < 4; i++) {
		if (!(rank_mask & (1 << i)))
			continue;

		select_per_cs_training_index(chan, i);

		/*
		 * disable PI_WDQLVL_VREF_EN before wdq leveling?
		 * PI_181 PI_WDQLVL_VREF_EN:RW:8:1
		 */
		clrbits_le32(&denali_pi[181], 0x1 << 8);

		/* PI_124 PI_WDQLVL_EN:RW:16:2 */
		clrsetbits_le32(&denali_pi[124], 0x3 << 16, 0x2 << 16);

		/* PI_121 PI_WDQLVL_REQ:WR:8:1,PI_WDQLVL_CS:RW:16:2 */
		clrsetbits_le32(&denali_pi[121],
				(0x1 << 8) | (0x3 << 16),
				(0x1 << 8) | (i << 16));

		/* Waiting for training complete */
		while (1) {
			/* PI_174 PI_INT_STATUS:RD:8:18 */
			tmp = readl(&denali_pi[174]) >> 8;
			if ((((tmp >> 12) & 0x1) == 0x1) &&
			    (((tmp >> 13) & 0x1) == 0x1) &&
			    (((tmp >> 6) & 0x1) == 0x0))
				break;
			else if (((tmp >> 6) & 0x1) == 0x1)
				return -EIO;
		}

		/* clear interrupt,PI_175 PI_INT_ACK:WR:0:17 */
		writel(0x00003f7c, (&denali_pi[175]));
	}

	clrbits_le32(&denali_pi[124], 0x3 << 16);

	return 0;
}

static int data_training(struct dram_info *dram, u32 channel,
			 const struct rk3399_sdram_params *params,
			 u32 training_flag)
{
	struct chan_info *chan = &dram->chan[channel];
	u32 *denali_phy = chan->publ->denali_phy;
	int ret;

	/* PHY_927 PHY_PAD_DQS_DRIVE  RPULL offset_22 */
	setbits_le32(&denali_phy[927], (1 << 22));

	if (training_flag == PI_FULL_TRAINING) {
		if (params->base.dramtype == LPDDR4) {
			training_flag = PI_WRITE_LEVELING |
					PI_READ_GATE_TRAINING |
					PI_READ_LEVELING | PI_WDQ_LEVELING;
		} else if (params->base.dramtype == LPDDR3) {
			training_flag = PI_CA_TRAINING | PI_WRITE_LEVELING |
					PI_READ_GATE_TRAINING;
		} else if (params->base.dramtype == DDR3) {
			training_flag = PI_WRITE_LEVELING |
					PI_READ_GATE_TRAINING |
					PI_READ_LEVELING;
		}
	}

	/* ca training(LPDDR4,LPDDR3 support) */
	if ((training_flag & PI_CA_TRAINING) == PI_CA_TRAINING) {
		ret = data_training_ca(chan, channel, params);
		if (ret < 0) {
			debug("%s: data training ca failed\n", __func__);
			return ret;
		}
	}

	/* write leveling(LPDDR4,LPDDR3,DDR3 support) */
	if ((training_flag & PI_WRITE_LEVELING) == PI_WRITE_LEVELING) {
		ret = data_training_wl(chan, channel, params);
		if (ret < 0) {
			debug("%s: data training wl failed\n", __func__);
			return ret;
		}
	}

	/* read gate training(LPDDR4,LPDDR3,DDR3 support) */
	if ((training_flag & PI_READ_GATE_TRAINING) == PI_READ_GATE_TRAINING) {
		ret = data_training_rg(chan, channel, params);
		if (ret < 0) {
			debug("%s: data training rg failed\n", __func__);
			return ret;
		}
	}

	/* read leveling(LPDDR4,LPDDR3,DDR3 support) */
	if ((training_flag & PI_READ_LEVELING) == PI_READ_LEVELING) {
		ret = data_training_rl(chan, channel, params);
		if (ret < 0) {
			debug("%s: data training rl failed\n", __func__);
			return ret;
		}
	}

	/* wdq leveling(LPDDR4 support) */
	if ((training_flag & PI_WDQ_LEVELING) == PI_WDQ_LEVELING) {
		ret = data_training_wdql(chan, channel, params);
		if (ret < 0) {
			debug("%s: data training wdql failed\n", __func__);
			return ret;
		}
	}

	/* PHY_927 PHY_PAD_DQS_DRIVE  RPULL offset_22 */
	clrbits_le32(&denali_phy[927], (1 << 22));

	return 0;
}

static void set_ddrconfig(const struct chan_info *chan,
			  const struct rk3399_sdram_params *params,
			  unsigned char channel, u32 ddrconfig)
{
	/* only need to set ddrconfig */
	struct rk3399_msch_regs *ddr_msch_regs = chan->msch;
	unsigned int cs0_cap = 0;
	unsigned int cs1_cap = 0;

	cs0_cap = (1 << (params->ch[channel].cap_info.cs0_row
			+ params->ch[channel].cap_info.col
			+ params->ch[channel].cap_info.bk
			+ params->ch[channel].cap_info.bw - 20));
	if (params->ch[channel].cap_info.rank > 1)
		cs1_cap = cs0_cap >> (params->ch[channel].cap_info.cs0_row
				- params->ch[channel].cap_info.cs1_row);
	if (params->ch[channel].cap_info.row_3_4) {
		cs0_cap = cs0_cap * 3 / 4;
		cs1_cap = cs1_cap * 3 / 4;
	}

	writel(ddrconfig | (ddrconfig << 8), &ddr_msch_regs->ddrconf);
	writel(((cs0_cap / 32) & 0xff) | (((cs1_cap / 32) & 0xff) << 8),
	       &ddr_msch_regs->ddrsize);
}

static void dram_all_config(struct dram_info *dram,
			    const struct rk3399_sdram_params *params)
{
	u32 sys_reg2 = 0;
	u32 sys_reg3 = 0;
	unsigned int channel, idx;

	sys_reg2 |= SYS_REG_ENC_DDRTYPE(params->base.dramtype);
	sys_reg2 |= SYS_REG_ENC_NUM_CH(params->base.num_channels);

	for (channel = 0, idx = 0;
	     (idx < params->base.num_channels) && (channel < 2);
	     channel++) {
		const struct rk3399_sdram_channel *info = &params->ch[channel];
		struct rk3399_msch_regs *ddr_msch_regs;
		const struct rk3399_msch_timings *noc_timing;

		if (params->ch[channel].cap_info.col == 0)
			continue;
		idx++;
		sys_reg2 |= SYS_REG_ENC_ROW_3_4(info->cap_info.row_3_4, channel);
		sys_reg2 |= SYS_REG_ENC_CHINFO(channel);
		sys_reg2 |= SYS_REG_ENC_RANK(info->cap_info.rank, channel);
		sys_reg2 |= SYS_REG_ENC_COL(info->cap_info.col, channel);
		sys_reg2 |= SYS_REG_ENC_BK(info->cap_info.bk, channel);
		sys_reg2 |= SYS_REG_ENC_BW(info->cap_info.bw, channel);
		sys_reg2 |= SYS_REG_ENC_DBW(info->cap_info.dbw, channel);
		SYS_REG_ENC_CS0_ROW(info->cap_info.cs0_row, sys_reg2, sys_reg3, channel);
		if (info->cap_info.cs1_row)
			SYS_REG_ENC_CS1_ROW(info->cap_info.cs1_row, sys_reg2,
					    sys_reg3, channel);
		sys_reg3 |= SYS_REG_ENC_CS1_COL(info->cap_info.col, channel);
		sys_reg3 |= SYS_REG_ENC_VERSION(DDR_SYS_REG_VERSION);

		ddr_msch_regs = dram->chan[channel].msch;
		noc_timing = &params->ch[channel].noc_timings;
		writel(noc_timing->ddrtiminga0,
		       &ddr_msch_regs->ddrtiminga0);
		writel(noc_timing->ddrtimingb0,
		       &ddr_msch_regs->ddrtimingb0);
		writel(noc_timing->ddrtimingc0.d32,
		       &ddr_msch_regs->ddrtimingc0);
		writel(noc_timing->devtodev0,
		       &ddr_msch_regs->devtodev0);
		writel(noc_timing->ddrmode.d32,
		       &ddr_msch_regs->ddrmode);

		/**
		 * rank 1 memory clock disable (dfi_dram_clk_disable = 1)
		 *
		 * The hardware for LPDDR4 with
		 * - CLK0P/N connect to lower 16-bits
		 * - CLK1P/N connect to higher 16-bits
		 *
		 * dfi dram clk is configured via CLK1P/N, so disabling
		 * dfi dram clk will disable the CLK1P/N as well for lpddr4.
		 */
		if (params->ch[channel].cap_info.rank == 1 &&
		    params->base.dramtype != LPDDR4)
			setbits_le32(&dram->chan[channel].pctl->denali_ctl[276],
				     1 << 17);
	}

	writel(sys_reg2, &dram->pmugrf->os_reg2);
	writel(sys_reg3, &dram->pmugrf->os_reg3);
	rk_clrsetreg(&dram->pmusgrf->soc_con4, 0x1f << 10,
		     params->base.stride << 10);

	/* reboot hold register set */
	writel(PRESET_SGRF_HOLD(0) | PRESET_GPIO0_HOLD(1) |
		PRESET_GPIO1_HOLD(1),
		&dram->pmucru->pmucru_rstnhold_con[1]);
	clrsetbits_le32(&dram->cru->glb_rst_con, 0x3, 0x3);
}

static void set_cap_relate_config(const struct chan_info *chan,
				  struct rk3399_sdram_params *params,
				  unsigned int channel)
{
	u32 *denali_ctl = chan->pctl->denali_ctl;
	u32 tmp;
	struct rk3399_msch_timings *noc_timing;

	if (params->base.dramtype == LPDDR3) {
		tmp = (8 << params->ch[channel].cap_info.bw) /
			(8 << params->ch[channel].cap_info.dbw);

		/**
		 * memdata_ratio
		 * 1 -> 0, 2 -> 1, 4 -> 2
		 */
		clrsetbits_le32(&denali_ctl[197], 0x7,
				(tmp >> 1));
		clrsetbits_le32(&denali_ctl[198], 0x7 << 8,
				(tmp >> 1) << 8);
	}

	noc_timing = &params->ch[channel].noc_timings;

	/*
	 * noc timing bw relate timing is 32 bit, and real bw is 16bit
	 * actually noc reg is setting at function dram_all_config
	 */
	if (params->ch[channel].cap_info.bw == 16 &&
	    noc_timing->ddrmode.b.mwrsize == 2) {
		if (noc_timing->ddrmode.b.burstsize)
			noc_timing->ddrmode.b.burstsize -= 1;
		noc_timing->ddrmode.b.mwrsize -= 1;
		noc_timing->ddrtimingc0.b.burstpenalty *= 2;
		noc_timing->ddrtimingc0.b.wrtomwr *= 2;
	}
}

static u32 calculate_ddrconfig(struct rk3399_sdram_params *params, u32 channel)
{
	unsigned int cs0_row = params->ch[channel].cap_info.cs0_row;
	unsigned int col = params->ch[channel].cap_info.col;
	unsigned int bw = params->ch[channel].cap_info.bw;
	u16  ddr_cfg_2_rbc[] = {
		/*
		 * [6]	  highest bit col
		 * [5:3]  max row(14+n)
		 * [2]    insertion row
		 * [1:0]  col(9+n),col, data bus 32bit
		 *
		 * highbitcol, max_row, insertion_row,  col
		 */
		((0 << 6) | (2 << 3) | (0 << 2) | 0), /* 0 */
		((0 << 6) | (2 << 3) | (0 << 2) | 1), /* 1 */
		((0 << 6) | (1 << 3) | (0 << 2) | 2), /* 2 */
		((0 << 6) | (0 << 3) | (0 << 2) | 3), /* 3 */
		((0 << 6) | (2 << 3) | (1 << 2) | 1), /* 4 */
		((0 << 6) | (1 << 3) | (1 << 2) | 2), /* 5 */
		((1 << 6) | (0 << 3) | (0 << 2) | 2), /* 6 */
		((1 << 6) | (1 << 3) | (0 << 2) | 2), /* 7 */
	};
	u32 i;

	col -= (bw == 2) ? 0 : 1;
	col -= 9;

	for (i = 0; i < 4; i++) {
		if ((col == (ddr_cfg_2_rbc[i] & 0x3)) &&
		    (cs0_row <= (((ddr_cfg_2_rbc[i] >> 3) & 0x7) + 14)))
			break;
	}

	if (i >= 4)
		i = -EINVAL;

	return i;
}

#if !defined(CONFIG_RAM_RK3399_LPDDR4)
static int default_data_training(struct dram_info *dram, u32 channel, u8 rank,
				 struct rk3399_sdram_params *params)
{
	u8 training_flag = PI_READ_GATE_TRAINING;

	/*
	 * LPDDR3 CA training msut be trigger before
	 * other training.
	 * DDR3 is not have CA training.
	 */

	if (params->base.dramtype == LPDDR3)
		training_flag |= PI_CA_TRAINING;

	return data_training(dram, channel, params, training_flag);
}

static int switch_to_phy_index1(struct dram_info *dram,
				struct rk3399_sdram_params *params)
{
	u32 channel;
	u32 *denali_phy;
	u32 ch_count = params->base.num_channels;
	int ret;
	int i = 0;

	writel(RK_CLRSETBITS(0x03 << 4 | 1 << 2 | 1,
			     1 << 4 | 1 << 2 | 1),
			&dram->cic->cic_ctrl0);
	while (!(readl(&dram->cic->cic_status0) & (1 << 2))) {
		mdelay(10);
		i++;
		if (i > 10) {
			debug("index1 frequency change overtime\n");
			return -ETIME;
		}
	}

	i = 0;
	writel(RK_CLRSETBITS(1 << 1, 1 << 1), &dram->cic->cic_ctrl0);
	while (!(readl(&dram->cic->cic_status0) & (1 << 0))) {
		mdelay(10);
		i++;
		if (i > 10) {
			debug("index1 frequency done overtime\n");
			return -ETIME;
		}
	}

	for (channel = 0; channel < ch_count; channel++) {
		denali_phy = dram->chan[channel].publ->denali_phy;
		clrsetbits_le32(&denali_phy[896], (0x3 << 8) | 1, 1 << 8);
		ret = data_training(dram, channel, params, PI_FULL_TRAINING);
		if (ret < 0) {
			debug("index1 training failed\n");
			return ret;
		}
	}

	return 0;
}

#else

struct rk3399_sdram_params lpddr4_timings[] = {
	#include "sdram-rk3399-lpddr4-400.inc"
	#include "sdram-rk3399-lpddr4-800.inc"
};

static void *get_denali_pi(const struct chan_info *chan,
			   struct rk3399_sdram_params *params, bool reg)
{
	return reg ? &chan->pi->denali_pi : &params->pi_regs.denali_pi;
}

static u32 lpddr4_get_phy(struct rk3399_sdram_params *params, u32 ctl)
{
	u32 lpddr4_phy[] = {1, 0, 0xb};

	return lpddr4_phy[ctl];
}

static u32 lpddr4_get_ctl(struct rk3399_sdram_params *params, u32 phy)
{
	u32 lpddr4_ctl[] = {1, 0, 2};

	return lpddr4_ctl[phy];
}

static u32 get_ddr_stride(struct rk3399_pmusgrf_regs *pmusgrf)
{
	return ((readl(&pmusgrf->soc_con4) >> 10) & 0x1F);
}

static void set_ddr_stride(struct rk3399_pmusgrf_regs *pmusgrf, u32 stride)
{
	rk_clrsetreg(&pmusgrf->soc_con4, 0x1f << 10, stride << 10);
}

/**
 * read mr_num mode register
 * rank = 1: cs0
 * rank = 2: cs1
 */
static int read_mr(struct rk3399_ddr_pctl_regs *ddr_pctl_regs, u32 rank,
		   u32 mr_num, u32 *buf)
{
	s32 timeout = 100;

	writel(((1 << 16) | (((rank == 2) ? 1 : 0) << 8) | mr_num) << 8,
	       &ddr_pctl_regs->denali_ctl[118]);

	while (0 == (readl(&ddr_pctl_regs->denali_ctl[203]) &
			((1 << 21) | (1 << 12)))) {
		udelay(1);

		if (timeout <= 0) {
			printf("%s: pctl timeout!\n", __func__);
			return -ETIMEDOUT;
		}

		timeout--;
	}

	if (!(readl(&ddr_pctl_regs->denali_ctl[203]) & (1 << 12))) {
		*buf = readl(&ddr_pctl_regs->denali_ctl[119]) & 0xFF;
	} else {
		printf("%s: read mr failed with 0x%x status\n", __func__,
		       readl(&ddr_pctl_regs->denali_ctl[17]) & 0x3);
		*buf = 0;
	}

	setbits_le32(&ddr_pctl_regs->denali_ctl[205], (1 << 21) | (1 << 12));

	return 0;
}

static int lpddr4_mr_detect(struct dram_info *dram, u32 channel, u8 rank,
			    struct rk3399_sdram_params *params)
{
	u64 cs0_cap;
	u32 stride;
	u32 cs = 0, col = 0, bk = 0, bw = 0, row_3_4 = 0;
	u32 cs0_row = 0, cs1_row = 0, ddrconfig = 0;
	u32 mr5, mr12, mr14;
	struct chan_info *chan = &dram->chan[channel];
	struct rk3399_ddr_pctl_regs *ddr_pctl_regs = chan->pctl;
	void __iomem *addr = NULL;
	int ret = 0;
	u32 val;

	stride = get_ddr_stride(dram->pmusgrf);

	if (params->ch[channel].cap_info.col == 0) {
		ret = -EPERM;
		goto end;
	}

	cs = params->ch[channel].cap_info.rank;
	col = params->ch[channel].cap_info.col;
	bk = params->ch[channel].cap_info.bk;
	bw = params->ch[channel].cap_info.bw;
	row_3_4 = params->ch[channel].cap_info.row_3_4;
	cs0_row = params->ch[channel].cap_info.cs0_row;
	cs1_row = params->ch[channel].cap_info.cs1_row;
	ddrconfig = params->ch[channel].cap_info.ddrconfig;

	/* 2GB */
	params->ch[channel].cap_info.rank = 2;
	params->ch[channel].cap_info.col = 10;
	params->ch[channel].cap_info.bk = 3;
	params->ch[channel].cap_info.bw = 2;
	params->ch[channel].cap_info.row_3_4 = 0;
	params->ch[channel].cap_info.cs0_row = 15;
	params->ch[channel].cap_info.cs1_row = 15;
	params->ch[channel].cap_info.ddrconfig = 1;

	set_memory_map(chan, channel, params);
	params->ch[channel].cap_info.ddrconfig =
			calculate_ddrconfig(params, channel);
	set_ddrconfig(chan, params, channel,
		      params->ch[channel].cap_info.ddrconfig);
	set_cap_relate_config(chan, params, channel);

	cs0_cap = (1 << (params->ch[channel].cap_info.bw
			+ params->ch[channel].cap_info.col
			+ params->ch[channel].cap_info.bk
			+ params->ch[channel].cap_info.cs0_row));

	if (params->ch[channel].cap_info.row_3_4)
		cs0_cap = cs0_cap * 3 / 4;

	if (channel == 0)
		set_ddr_stride(dram->pmusgrf, 0x17);
	else
		set_ddr_stride(dram->pmusgrf, 0x18);

	/* read and write data to DRAM, avoid be optimized by compiler. */
	if (rank == 1)
		addr = (void __iomem *)0x100;
	else if (rank == 2)
		addr = (void __iomem *)(cs0_cap + 0x100);

	val = readl(addr);
	writel(val + 1, addr);

	read_mr(ddr_pctl_regs, rank, 5, &mr5);
	read_mr(ddr_pctl_regs, rank, 12, &mr12);
	read_mr(ddr_pctl_regs, rank, 14, &mr14);

	if (mr5 == 0 || mr12 != 0x4d || mr14 != 0x4d) {
		ret = -EINVAL;
		goto end;
	}
end:
	params->ch[channel].cap_info.rank = cs;
	params->ch[channel].cap_info.col = col;
	params->ch[channel].cap_info.bk = bk;
	params->ch[channel].cap_info.bw = bw;
	params->ch[channel].cap_info.row_3_4 = row_3_4;
	params->ch[channel].cap_info.cs0_row = cs0_row;
	params->ch[channel].cap_info.cs1_row = cs1_row;
	params->ch[channel].cap_info.ddrconfig = ddrconfig;

	set_ddr_stride(dram->pmusgrf, stride);

	return ret;
}

static void set_lpddr4_dq_odt(const struct chan_info *chan,
			      struct rk3399_sdram_params *params, u32 ctl,
			      bool en, bool ctl_phy_reg, u32 mr5)
{
	u32 *denali_ctl = get_denali_ctl(chan, params, ctl_phy_reg);
	u32 *denali_pi = get_denali_pi(chan, params, ctl_phy_reg);
	struct io_setting *io;
	u32 reg_value;

	if (!en)
		return;

	io = lpddr4_get_io_settings(params, mr5);

	reg_value = io->dq_odt;

	switch (ctl) {
	case 0:
		clrsetbits_le32(&denali_ctl[139], 0x7 << 24, reg_value << 24);
		clrsetbits_le32(&denali_ctl[153], 0x7 << 24, reg_value << 24);

		clrsetbits_le32(&denali_pi[132], 0x7 << 0, (reg_value << 0));
		clrsetbits_le32(&denali_pi[139], 0x7 << 16, (reg_value << 16));
		clrsetbits_le32(&denali_pi[147], 0x7 << 0, (reg_value << 0));
		clrsetbits_le32(&denali_pi[154], 0x7 << 16, (reg_value << 16));
		break;
	case 1:
		clrsetbits_le32(&denali_ctl[140], 0x7 << 0, reg_value << 0);
		clrsetbits_le32(&denali_ctl[154], 0x7 << 0, reg_value << 0);

		clrsetbits_le32(&denali_pi[129], 0x7 << 16, (reg_value << 16));
		clrsetbits_le32(&denali_pi[137], 0x7 << 0, (reg_value << 0));
		clrsetbits_le32(&denali_pi[144], 0x7 << 16, (reg_value << 16));
		clrsetbits_le32(&denali_pi[152], 0x7 << 0, (reg_value << 0));
		break;
	case 2:
	default:
		clrsetbits_le32(&denali_ctl[140], 0x7 << 8, (reg_value << 8));
		clrsetbits_le32(&denali_ctl[154], 0x7 << 8, (reg_value << 8));

		clrsetbits_le32(&denali_pi[127], 0x7 << 0, (reg_value << 0));
		clrsetbits_le32(&denali_pi[134], 0x7 << 16, (reg_value << 16));
		clrsetbits_le32(&denali_pi[142], 0x7 << 0, (reg_value << 0));
		clrsetbits_le32(&denali_pi[149], 0x7 << 16, (reg_value << 16));
		break;
	}
}

static void set_lpddr4_ca_odt(const struct chan_info *chan,
			      struct rk3399_sdram_params *params, u32 ctl,
			      bool en, bool ctl_phy_reg, u32 mr5)
{
	u32 *denali_ctl = get_denali_ctl(chan, params, ctl_phy_reg);
	u32 *denali_pi = get_denali_pi(chan, params, ctl_phy_reg);
	struct io_setting *io;
	u32 reg_value;

	if (!en)
		return;

	io = lpddr4_get_io_settings(params, mr5);

	reg_value = io->ca_odt;

	switch (ctl) {
	case 0:
		clrsetbits_le32(&denali_ctl[139], 0x7 << 28, reg_value << 28);
		clrsetbits_le32(&denali_ctl[153], 0x7 << 28, reg_value << 28);

		clrsetbits_le32(&denali_pi[132], 0x7 << 4, reg_value << 4);
		clrsetbits_le32(&denali_pi[139], 0x7 << 20, reg_value << 20);
		clrsetbits_le32(&denali_pi[147], 0x7 << 4, reg_value << 4);
		clrsetbits_le32(&denali_pi[154], 0x7 << 20, reg_value << 20);
		break;
	case 1:
		clrsetbits_le32(&denali_ctl[140], 0x7 << 4, reg_value << 4);
		clrsetbits_le32(&denali_ctl[154], 0x7 << 4, reg_value << 4);

		clrsetbits_le32(&denali_pi[129], 0x7 << 20, reg_value << 20);
		clrsetbits_le32(&denali_pi[137], 0x7 << 4, reg_value << 4);
		clrsetbits_le32(&denali_pi[144], 0x7 << 20, reg_value << 20);
		clrsetbits_le32(&denali_pi[152], 0x7 << 4, reg_value << 4);
		break;
	case 2:
	default:
		clrsetbits_le32(&denali_ctl[140], 0x7 << 12, (reg_value << 12));
		clrsetbits_le32(&denali_ctl[154], 0x7 << 12, (reg_value << 12));

		clrsetbits_le32(&denali_pi[127], 0x7 << 4, reg_value << 4);
		clrsetbits_le32(&denali_pi[134], 0x7 << 20, reg_value << 20);
		clrsetbits_le32(&denali_pi[142], 0x7 << 4, reg_value << 4);
		clrsetbits_le32(&denali_pi[149], 0x7 << 20, reg_value << 20);
		break;
	}
}

static void set_lpddr4_MR3(const struct chan_info *chan,
			   struct rk3399_sdram_params *params, u32 ctl,
			   bool ctl_phy_reg, u32 mr5)
{
	u32 *denali_ctl = get_denali_ctl(chan, params, ctl_phy_reg);
	u32 *denali_pi = get_denali_pi(chan, params, ctl_phy_reg);
	struct io_setting *io;
	u32 reg_value;

	io = lpddr4_get_io_settings(params, mr5);

	reg_value = ((io->pdds << 3) | 1);

	switch (ctl) {
	case 0:
		clrsetbits_le32(&denali_ctl[138], 0xFFFF, reg_value);
		clrsetbits_le32(&denali_ctl[152], 0xFFFF, reg_value);

		clrsetbits_le32(&denali_pi[131], 0xFFFF << 16, reg_value << 16);
		clrsetbits_le32(&denali_pi[139], 0xFFFF, reg_value);
		clrsetbits_le32(&denali_pi[146], 0xFFFF << 16, reg_value << 16);
		clrsetbits_le32(&denali_pi[154], 0xFFFF, reg_value);
		break;
	case 1:
		clrsetbits_le32(&denali_ctl[138], 0xFFFF << 16,
				reg_value << 16);
		clrsetbits_le32(&denali_ctl[152], 0xFFFF << 16,
				reg_value << 16);

		clrsetbits_le32(&denali_pi[129], 0xFFFF, reg_value);
		clrsetbits_le32(&denali_pi[136], 0xFFFF << 16, reg_value << 16);
		clrsetbits_le32(&denali_pi[144], 0xFFFF, reg_value);
		clrsetbits_le32(&denali_pi[151], 0xFFFF << 16, reg_value << 16);
		break;
	case 2:
	default:
		clrsetbits_le32(&denali_ctl[139], 0xFFFF, reg_value);
		clrsetbits_le32(&denali_ctl[153], 0xFFFF, reg_value);

		clrsetbits_le32(&denali_pi[126], 0xFFFF << 16, reg_value << 16);
		clrsetbits_le32(&denali_pi[134], 0xFFFF, reg_value);
		clrsetbits_le32(&denali_pi[141], 0xFFFF << 16, reg_value << 16);
		clrsetbits_le32(&denali_pi[149], 0xFFFF, reg_value);
		break;
	}
}

static void set_lpddr4_MR12(const struct chan_info *chan,
			    struct rk3399_sdram_params *params, u32 ctl,
			    bool ctl_phy_reg, u32 mr5)
{
	u32 *denali_ctl = get_denali_ctl(chan, params, ctl_phy_reg);
	u32 *denali_pi = get_denali_pi(chan, params, ctl_phy_reg);
	struct io_setting *io;
	u32 reg_value;

	io = lpddr4_get_io_settings(params, mr5);

	reg_value = io->ca_vref;

	switch (ctl) {
	case 0:
		clrsetbits_le32(&denali_ctl[140], 0xFFFF << 16,
				reg_value << 16);
		clrsetbits_le32(&denali_ctl[154], 0xFFFF << 16,
				reg_value << 16);

		clrsetbits_le32(&denali_pi[132], 0xFF << 8, reg_value << 8);
		clrsetbits_le32(&denali_pi[139], 0xFF << 24, reg_value << 24);
		clrsetbits_le32(&denali_pi[147], 0xFF << 8, reg_value << 8);
		clrsetbits_le32(&denali_pi[154], 0xFF << 24, reg_value << 24);
		break;
	case 1:
		clrsetbits_le32(&denali_ctl[141], 0xFFFF, reg_value);
		clrsetbits_le32(&denali_ctl[155], 0xFFFF, reg_value);

		clrsetbits_le32(&denali_pi[129], 0xFF << 24, reg_value << 24);
		clrsetbits_le32(&denali_pi[137], 0xFF << 8, reg_value << 8);
		clrsetbits_le32(&denali_pi[144], 0xFF << 24, reg_value << 24);
		clrsetbits_le32(&denali_pi[152], 0xFF << 8, reg_value << 8);
		break;
	case 2:
	default:
		clrsetbits_le32(&denali_ctl[141], 0xFFFF << 16,
				reg_value << 16);
		clrsetbits_le32(&denali_ctl[155], 0xFFFF << 16,
				reg_value << 16);

		clrsetbits_le32(&denali_pi[127], 0xFF << 8, reg_value << 8);
		clrsetbits_le32(&denali_pi[134], 0xFF << 24, reg_value << 24);
		clrsetbits_le32(&denali_pi[142], 0xFF << 8, reg_value << 8);
		clrsetbits_le32(&denali_pi[149], 0xFF << 24, reg_value << 24);
		break;
	}
}

static void set_lpddr4_MR14(const struct chan_info *chan,
			    struct rk3399_sdram_params *params, u32 ctl,
			    bool ctl_phy_reg, u32 mr5)
{
	u32 *denali_ctl = get_denali_ctl(chan, params, ctl_phy_reg);
	u32 *denali_pi = get_denali_pi(chan, params, ctl_phy_reg);
	struct io_setting *io;
	u32 reg_value;

	io = lpddr4_get_io_settings(params, mr5);

	reg_value = io->dq_vref;

	switch (ctl) {
	case 0:
		clrsetbits_le32(&denali_ctl[142], 0xFFFF << 16,
				reg_value << 16);
		clrsetbits_le32(&denali_ctl[156], 0xFFFF << 16,
				reg_value << 16);

		clrsetbits_le32(&denali_pi[132], 0xFF << 16, reg_value << 16);
		clrsetbits_le32(&denali_pi[140], 0xFF << 0, reg_value << 0);
		clrsetbits_le32(&denali_pi[147], 0xFF << 16, reg_value << 16);
		clrsetbits_le32(&denali_pi[155], 0xFF << 0, reg_value << 0);
		break;
	case 1:
		clrsetbits_le32(&denali_ctl[143], 0xFFFF, reg_value);
		clrsetbits_le32(&denali_ctl[157], 0xFFFF, reg_value);

		clrsetbits_le32(&denali_pi[130], 0xFF << 0, reg_value << 0);
		clrsetbits_le32(&denali_pi[137], 0xFF << 16, reg_value << 16);
		clrsetbits_le32(&denali_pi[145], 0xFF << 0, reg_value << 0);
		clrsetbits_le32(&denali_pi[152], 0xFF << 16, reg_value << 16);
		break;
	case 2:
	default:
		clrsetbits_le32(&denali_ctl[143], 0xFFFF << 16,
				reg_value << 16);
		clrsetbits_le32(&denali_ctl[157], 0xFFFF << 16,
				reg_value << 16);

		clrsetbits_le32(&denali_pi[127], 0xFF << 16, reg_value << 16);
		clrsetbits_le32(&denali_pi[135], 0xFF << 0, reg_value << 0);
		clrsetbits_le32(&denali_pi[142], 0xFF << 16, reg_value << 16);
		clrsetbits_le32(&denali_pi[150], 0xFF << 0, reg_value << 0);
		break;
	}
}

static void lpddr4_copy_phy(struct dram_info *dram,
			    struct rk3399_sdram_params *params, u32 phy,
			    struct rk3399_sdram_params *timings,
			    u32 channel)
{
	u32 *denali_ctl, *denali_phy;
	u32 *denali_phy_params;
	u32 speed = 0;
	u32 ctl, mr5;

	denali_ctl = dram->chan[channel].pctl->denali_ctl;
	denali_phy = dram->chan[channel].publ->denali_phy;
	denali_phy_params = timings->phy_regs.denali_phy;

	/* switch index */
	clrsetbits_le32(&denali_phy_params[896], 0x3 << 8, phy << 8);
	writel(denali_phy_params[896], &denali_phy[896]);

	/* phy_pll_ctrl_ca, phy_pll_ctrl */
	writel(denali_phy_params[911], &denali_phy[911]);

	/* phy_low_freq_sel */
	clrsetbits_le32(&denali_phy[913], 0x1,
			denali_phy_params[913] & 0x1);

	/* phy_grp_slave_delay_x, phy_cslvl_dly_step */
	writel(denali_phy_params[916], &denali_phy[916]);
	writel(denali_phy_params[917], &denali_phy[917]);
	writel(denali_phy_params[918], &denali_phy[918]);

	/* phy_adrz_sw_wraddr_shift_x  */
	writel(denali_phy_params[512], &denali_phy[512]);
	clrsetbits_le32(&denali_phy[513], 0xffff,
			denali_phy_params[513] & 0xffff);
	writel(denali_phy_params[640], &denali_phy[640]);
	clrsetbits_le32(&denali_phy[641], 0xffff,
			denali_phy_params[641] & 0xffff);
	writel(denali_phy_params[768], &denali_phy[768]);
	clrsetbits_le32(&denali_phy[769], 0xffff,
			denali_phy_params[769] & 0xffff);

	writel(denali_phy_params[544], &denali_phy[544]);
	writel(denali_phy_params[545], &denali_phy[545]);
	writel(denali_phy_params[546], &denali_phy[546]);
	writel(denali_phy_params[547], &denali_phy[547]);

	writel(denali_phy_params[672], &denali_phy[672]);
	writel(denali_phy_params[673], &denali_phy[673]);
	writel(denali_phy_params[674], &denali_phy[674]);
	writel(denali_phy_params[675], &denali_phy[675]);

	writel(denali_phy_params[800], &denali_phy[800]);
	writel(denali_phy_params[801], &denali_phy[801]);
	writel(denali_phy_params[802], &denali_phy[802]);
	writel(denali_phy_params[803], &denali_phy[803]);

	/*
	 * phy_adr_master_delay_start_x
	 * phy_adr_master_delay_step_x
	 * phy_adr_master_delay_wait_x
	 */
	writel(denali_phy_params[548], &denali_phy[548]);
	writel(denali_phy_params[676], &denali_phy[676]);
	writel(denali_phy_params[804], &denali_phy[804]);

	/* phy_adr_calvl_dly_step_x */
	writel(denali_phy_params[549], &denali_phy[549]);
	writel(denali_phy_params[677], &denali_phy[677]);
	writel(denali_phy_params[805], &denali_phy[805]);

	/*
	 * phy_clk_wrdm_slave_delay_x
	 * phy_clk_wrdqz_slave_delay_x
	 * phy_clk_wrdqs_slave_delay_x
	 */
	copy_to_reg((u32 *)&denali_phy[59], (u32 *)&denali_phy_params[59],
		    (63 - 58) * 4);
	copy_to_reg((u32 *)&denali_phy[187], (u32 *)&denali_phy_params[187],
		    (191 - 186) * 4);
	copy_to_reg((u32 *)&denali_phy[315], (u32 *)&denali_phy_params[315],
		    (319 - 314) * 4);
	copy_to_reg((u32 *)&denali_phy[443], (u32 *)&denali_phy_params[443],
		    (447 - 442) * 4);

	/*
	 * phy_dqs_tsel_wr_timing_x 8bits denali_phy_84/212/340/468 offset_8
	 * dqs_tsel_wr_end[7:4] add half cycle
	 * phy_dq_tsel_wr_timing_x 8bits denali_phy_83/211/339/467 offset_8
	 * dq_tsel_wr_end[7:4] add half cycle
	 */
	writel(denali_phy_params[83] + (0x10 << 16), &denali_phy[83]);
	writel(denali_phy_params[84] + (0x10 << 8), &denali_phy[84]);
	writel(denali_phy_params[85], &denali_phy[85]);

	writel(denali_phy_params[211] + (0x10 << 16), &denali_phy[211]);
	writel(denali_phy_params[212] + (0x10 << 8), &denali_phy[212]);
	writel(denali_phy_params[213], &denali_phy[213]);

	writel(denali_phy_params[339] + (0x10 << 16), &denali_phy[339]);
	writel(denali_phy_params[340] + (0x10 << 8), &denali_phy[340]);
	writel(denali_phy_params[341], &denali_phy[341]);

	writel(denali_phy_params[467] + (0x10 << 16), &denali_phy[467]);
	writel(denali_phy_params[468] + (0x10 << 8), &denali_phy[468]);
	writel(denali_phy_params[469], &denali_phy[469]);

	/*
	 * phy_gtlvl_resp_wait_cnt_x
	 * phy_gtlvl_dly_step_x
	 * phy_wrlvl_resp_wait_cnt_x
	 * phy_gtlvl_final_step_x
	 * phy_gtlvl_back_step_x
	 * phy_rdlvl_dly_step_x
	 *
	 * phy_master_delay_step_x
	 * phy_master_delay_wait_x
	 * phy_wrlvl_dly_step_x
	 * phy_rptr_update_x
	 * phy_wdqlvl_dly_step_x
	 */
	writel(denali_phy_params[87], &denali_phy[87]);
	writel(denali_phy_params[88], &denali_phy[88]);
	writel(denali_phy_params[89], &denali_phy[89]);
	writel(denali_phy_params[90], &denali_phy[90]);

	writel(denali_phy_params[215], &denali_phy[215]);
	writel(denali_phy_params[216], &denali_phy[216]);
	writel(denali_phy_params[217], &denali_phy[217]);
	writel(denali_phy_params[218], &denali_phy[218]);

	writel(denali_phy_params[343], &denali_phy[343]);
	writel(denali_phy_params[344], &denali_phy[344]);
	writel(denali_phy_params[345], &denali_phy[345]);
	writel(denali_phy_params[346], &denali_phy[346]);

	writel(denali_phy_params[471], &denali_phy[471]);
	writel(denali_phy_params[472], &denali_phy[472]);
	writel(denali_phy_params[473], &denali_phy[473]);
	writel(denali_phy_params[474], &denali_phy[474]);

	/*
	 * phy_gtlvl_lat_adj_start_x
	 * phy_gtlvl_rddqs_slv_dly_start_x
	 * phy_rdlvl_rddqs_dq_slv_dly_start_x
	 * phy_wdqlvl_dqdm_slv_dly_start_x
	 */
	writel(denali_phy_params[80], &denali_phy[80]);
	writel(denali_phy_params[81], &denali_phy[81]);

	writel(denali_phy_params[208], &denali_phy[208]);
	writel(denali_phy_params[209], &denali_phy[209]);

	writel(denali_phy_params[336], &denali_phy[336]);
	writel(denali_phy_params[337], &denali_phy[337]);

	writel(denali_phy_params[464], &denali_phy[464]);
	writel(denali_phy_params[465], &denali_phy[465]);

	/*
	 * phy_master_delay_start_x
	 * phy_sw_master_mode_x
	 * phy_rddata_en_tsel_dly_x
	 */
	writel(denali_phy_params[86], &denali_phy[86]);
	writel(denali_phy_params[214], &denali_phy[214]);
	writel(denali_phy_params[342], &denali_phy[342]);
	writel(denali_phy_params[470], &denali_phy[470]);

	/*
	 * phy_rddqz_slave_delay_x
	 * phy_rddqs_dqz_fall_slave_delay_x
	 * phy_rddqs_dqz_rise_slave_delay_x
	 * phy_rddqs_dm_fall_slave_delay_x
	 * phy_rddqs_dm_rise_slave_delay_x
	 * phy_rddqs_gate_slave_delay_x
	 * phy_wrlvl_delay_early_threshold_x
	 * phy_write_path_lat_add_x
	 * phy_rddqs_latency_adjust_x
	 * phy_wrlvl_delay_period_threshold_x
	 * phy_wrlvl_early_force_zero_x
	 */
	copy_to_reg((u32 *)&denali_phy[64], (u32 *)&denali_phy_params[64],
		    (67 - 63) * 4);
	clrsetbits_le32(&denali_phy[68], 0xfffffc00,
			denali_phy_params[68] & 0xfffffc00);
	copy_to_reg((u32 *)&denali_phy[69], (u32 *)&denali_phy_params[69],
		    (79 - 68) * 4);
	copy_to_reg((u32 *)&denali_phy[192], (u32 *)&denali_phy_params[192],
		    (195 - 191) * 4);
	clrsetbits_le32(&denali_phy[196], 0xfffffc00,
			denali_phy_params[196] & 0xfffffc00);
	copy_to_reg((u32 *)&denali_phy[197], (u32 *)&denali_phy_params[197],
		    (207 - 196) * 4);
	copy_to_reg((u32 *)&denali_phy[320], (u32 *)&denali_phy_params[320],
		    (323 - 319) * 4);
	clrsetbits_le32(&denali_phy[324], 0xfffffc00,
			denali_phy_params[324] & 0xfffffc00);
	copy_to_reg((u32 *)&denali_phy[325], (u32 *)&denali_phy_params[325],
		    (335 - 324) * 4);

	copy_to_reg((u32 *)&denali_phy[448], (u32 *)&denali_phy_params[448],
		    (451 - 447) * 4);
	clrsetbits_le32(&denali_phy[452], 0xfffffc00,
			denali_phy_params[452] & 0xfffffc00);
	copy_to_reg((u32 *)&denali_phy[453], (u32 *)&denali_phy_params[453],
		    (463 - 452) * 4);

	/* phy_two_cyc_preamble_x */
	clrsetbits_le32(&denali_phy[7], 0x3 << 24,
			denali_phy_params[7] & (0x3 << 24));
	clrsetbits_le32(&denali_phy[135], 0x3 << 24,
			denali_phy_params[135] & (0x3 << 24));
	clrsetbits_le32(&denali_phy[263], 0x3 << 24,
			denali_phy_params[263] & (0x3 << 24));
	clrsetbits_le32(&denali_phy[391], 0x3 << 24,
			denali_phy_params[391] & (0x3 << 24));

	/* speed */
	if (timings->base.ddr_freq < 400 * MHz)
		speed = 0x0;
	else if (timings->base.ddr_freq < 800 * MHz)
		speed = 0x1;
	else if (timings->base.ddr_freq < 1200 * MHz)
		speed = 0x2;

	/* phy_924 phy_pad_fdbk_drive */
	clrsetbits_le32(&denali_phy[924], 0x3 << 21, speed << 21);
	/* phy_926 phy_pad_data_drive */
	clrsetbits_le32(&denali_phy[926], 0x3 << 9, speed << 9);
	/* phy_927 phy_pad_dqs_drive */
	clrsetbits_le32(&denali_phy[927], 0x3 << 9, speed << 9);
	/* phy_928 phy_pad_addr_drive */
	clrsetbits_le32(&denali_phy[928], 0x3 << 17, speed << 17);
	/* phy_929 phy_pad_clk_drive */
	clrsetbits_le32(&denali_phy[929], 0x3 << 17, speed << 17);
	/* phy_935 phy_pad_cke_drive */
	clrsetbits_le32(&denali_phy[935], 0x3 << 17, speed << 17);
	/* phy_937 phy_pad_rst_drive */
	clrsetbits_le32(&denali_phy[937], 0x3 << 17, speed << 17);
	/* phy_939 phy_pad_cs_drive */
	clrsetbits_le32(&denali_phy[939], 0x3 << 17, speed << 17);

	read_mr(dram->chan[channel].pctl, 1, 5, &mr5);
	set_ds_odt(&dram->chan[channel], timings, true, mr5);

	ctl = lpddr4_get_ctl(timings, phy);
	set_lpddr4_dq_odt(&dram->chan[channel], timings, ctl, true, true, mr5);
	set_lpddr4_ca_odt(&dram->chan[channel], timings, ctl, true, true, mr5);
	set_lpddr4_MR3(&dram->chan[channel], timings, ctl, true, mr5);
	set_lpddr4_MR12(&dram->chan[channel], timings, ctl, true, mr5);
	set_lpddr4_MR14(&dram->chan[channel], timings, ctl, true, mr5);

	/*
	 * if phy_sw_master_mode_x not bypass mode,
	 * clear phy_slice_pwr_rdc_disable.
	 * note: need use timings, not ddr_publ_regs
	 */
	if (!((denali_phy_params[86] >> 8) & (1 << 2))) {
		clrbits_le32(&denali_phy[10], 1 << 16);
		clrbits_le32(&denali_phy[138], 1 << 16);
		clrbits_le32(&denali_phy[266], 1 << 16);
		clrbits_le32(&denali_phy[394], 1 << 16);
	}

	/*
	 * when PHY_PER_CS_TRAINING_EN=1, W2W_DIFFCS_DLY_Fx can't
	 * smaller than 8
	 * NOTE: need use timings, not ddr_publ_regs
	 */
	if ((denali_phy_params[84] >> 16) & 1) {
		if (((readl(&denali_ctl[217 + ctl]) >> 16) & 0x1f) < 8)
			clrsetbits_le32(&denali_ctl[217 + ctl],
					0x1f << 16, 8 << 16);
	}
}

static void lpddr4_set_phy(struct dram_info *dram,
			   struct rk3399_sdram_params *params, u32 phy,
			   struct rk3399_sdram_params *timings)
{
	u32 channel;

	for (channel = 0; channel < 2; channel++)
		lpddr4_copy_phy(dram, params, phy, timings, channel);
}

static int lpddr4_set_ctl(struct dram_info *dram,
			  struct rk3399_sdram_params *params, u32 ctl, u32 hz)
{
	u32 channel;
	int ret_clk, ret;

	/* cci idle req stall */
	writel(0x70007, &dram->grf->soc_con0);

	/* enable all clk */
	setbits_le32(&dram->pmu->pmu_noc_auto_ena, (0x3 << 7));

	/* idle */
	setbits_le32(&dram->pmu->pmu_bus_idle_req, (0x3 << 18));
	while ((readl(&dram->pmu->pmu_bus_idle_st) & (0x3 << 18))
	       != (0x3 << 18))
		;

	/* change freq */
	writel((((0x3 << 4) | (1 << 2) | 1) << 16) |
		(ctl << 4) | (1 << 2) | 1, &dram->cic->cic_ctrl0);
	while (!(readl(&dram->cic->cic_status0) & (1 << 2)))
		;

	ret_clk = clk_set_rate(&dram->ddr_clk, hz);
	if (ret_clk < 0) {
		printf("%s clk set failed %d\n", __func__, ret_clk);
		return ret_clk;
	}

	writel(0x20002, &dram->cic->cic_ctrl0);
	while (!(readl(&dram->cic->cic_status0) & (1 << 0)))
		;

	/* deidle */
	clrbits_le32(&dram->pmu->pmu_bus_idle_req, (0x3 << 18));
	while (readl(&dram->pmu->pmu_bus_idle_st) & (0x3 << 18))
		;

	/* clear enable all clk */
	clrbits_le32(&dram->pmu->pmu_noc_auto_ena, (0x3 << 7));

	/* lpddr4 ctl2 can not do training, all training will fail */
	if (!(params->base.dramtype == LPDDR4 && ctl == 2)) {
		for (channel = 0; channel < 2; channel++) {
			if (!(params->ch[channel].cap_info.col))
				continue;
			ret = data_training(dram, channel, params,
						     PI_FULL_TRAINING);
			if (ret)
				printf("%s: channel %d training failed!\n",
				       __func__, channel);
			else
				debug("%s: channel %d training pass\n",
				      __func__, channel);
		}
	}

	return 0;
}

static int lpddr4_set_rate(struct dram_info *dram,
			   struct rk3399_sdram_params *params)
{
	u32 ctl;
	u32 phy;

	for (ctl = 0; ctl < 2; ctl++) {
		phy = lpddr4_get_phy(params, ctl);

		lpddr4_set_phy(dram, params, phy, &lpddr4_timings[ctl]);
		lpddr4_set_ctl(dram, params, ctl,
			       lpddr4_timings[ctl].base.ddr_freq);

		debug("%s: change freq to %d mhz %d, %d\n", __func__,
		      lpddr4_timings[ctl].base.ddr_freq / MHz, ctl, phy);
	}

	return 0;
}
#endif /* CONFIG_RAM_RK3399_LPDDR4 */

static unsigned char calculate_stride(struct rk3399_sdram_params *params)
{
	unsigned int stride = params->base.stride;
	unsigned int channel, chinfo = 0;
	unsigned int ch_cap[2] = {0, 0};
	u64 cap;

	for (channel = 0; channel < 2; channel++) {
		unsigned int cs0_cap = 0;
		unsigned int cs1_cap = 0;
		struct sdram_cap_info *cap_info = &params->ch[channel].cap_info;

		if (cap_info->col == 0)
			continue;

		cs0_cap = (1 << (cap_info->cs0_row + cap_info->col +
				 cap_info->bk + cap_info->bw - 20));
		if (cap_info->rank > 1)
			cs1_cap = cs0_cap >> (cap_info->cs0_row
					      - cap_info->cs1_row);
		if (cap_info->row_3_4) {
			cs0_cap = cs0_cap * 3 / 4;
			cs1_cap = cs1_cap * 3 / 4;
		}
		ch_cap[channel] = cs0_cap + cs1_cap;
		chinfo |= 1 << channel;
	}

	/* stride calculation for 1 channel */
	if (params->base.num_channels == 1 && chinfo & 1)
		return 0x17;	/* channel a */

	/* stride calculation for 2 channels, default gstride type is 256B */
	if (ch_cap[0] == ch_cap[1]) {
		cap = ch_cap[0] + ch_cap[1];
		switch (cap) {
		/* 512MB */
		case 512:
			stride = 0;
			break;
		/* 1GB */
		case 1024:
			stride = 0x5;
			break;
		/*
		 * 768MB + 768MB same as total 2GB memory
		 * useful space: 0-768MB 1GB-1792MB
		 */
		case 1536:
		/* 2GB */
		case 2048:
			stride = 0x9;
			break;
		/* 1536MB + 1536MB */
		case 3072:
			stride = 0x11;
			break;
		/* 4GB */
		case 4096:
			stride = 0xD;
			break;
		default:
			printf("%s: Unable to calculate stride for ", __func__);
			print_size((cap * (1 << 20)), " capacity\n");
			break;
		}
	}

	sdram_print_stride(stride);

	return stride;
}

static void clear_channel_params(struct rk3399_sdram_params *params, u8 channel)
{
	params->ch[channel].cap_info.rank = 0;
	params->ch[channel].cap_info.col = 0;
	params->ch[channel].cap_info.bk = 0;
	params->ch[channel].cap_info.bw = 32;
	params->ch[channel].cap_info.dbw = 32;
	params->ch[channel].cap_info.row_3_4 = 0;
	params->ch[channel].cap_info.cs0_row = 0;
	params->ch[channel].cap_info.cs1_row = 0;
	params->ch[channel].cap_info.ddrconfig = 0;
}

static int pctl_init(struct dram_info *dram, struct rk3399_sdram_params *params)
{
	int channel;
	int ret;

	for (channel = 0; channel < 2; channel++) {
		const struct chan_info *chan = &dram->chan[channel];
		struct rk3399_cru *cru = dram->cru;
		struct rk3399_ddr_publ_regs *publ = chan->publ;

		phy_pctrl_reset(cru, channel);
		phy_dll_bypass_set(publ, params->base.ddr_freq);

		ret = pctl_cfg(dram, chan, channel, params);
		if (ret < 0) {
			printf("%s: pctl config failed\n", __func__);
			return ret;
		}

		/* start to trigger initialization */
		pctl_start(dram, channel);
	}

	return 0;
}

static int sdram_init(struct dram_info *dram,
		      struct rk3399_sdram_params *params)
{
	unsigned char dramtype = params->base.dramtype;
	unsigned int ddr_freq = params->base.ddr_freq;
	int channel, ch, rank;
	int ret;

	debug("Starting SDRAM initialization...\n");

	if ((dramtype == DDR3 && ddr_freq > 933) ||
	    (dramtype == LPDDR3 && ddr_freq > 933) ||
	    (dramtype == LPDDR4 && ddr_freq > 800)) {
		debug("SDRAM frequency is to high!");
		return -E2BIG;
	}

	for (ch = 0; ch < 2; ch++) {
		params->ch[ch].cap_info.rank = 2;
		for (rank = 2; rank != 0; rank--) {
			ret = pctl_init(dram, params);
			if (ret < 0) {
				printf("%s: pctl init failed\n", __func__);
				return ret;
			}

			/* LPDDR2/LPDDR3 need to wait DAI complete, max 10us */
			if (dramtype == LPDDR3)
				udelay(10);

			params->ch[ch].cap_info.rank = rank;

			ret = dram->ops->data_training(dram, ch, rank, params);
			if (!ret) {
				debug("%s: data trained for rank %d, ch %d\n",
				      __func__, rank, ch);
				break;
			}
		}
		/* Computed rank with associated channel number */
		params->ch[ch].cap_info.rank = rank;
	}

	params->base.num_channels = 0;
	for (channel = 0; channel < 2; channel++) {
		const struct chan_info *chan = &dram->chan[channel];
		struct sdram_cap_info *cap_info = &params->ch[channel].cap_info;
		u8 training_flag = PI_FULL_TRAINING;

		if (cap_info->rank == 0) {
			clear_channel_params(params, channel);
			continue;
		} else {
			params->base.num_channels++;
		}

		debug("Channel ");
		debug(channel ? "1: " : "0: ");

		/* LPDDR3 should have write and read gate training */
		if (params->base.dramtype == LPDDR3)
			training_flag = PI_WRITE_LEVELING |
					PI_READ_GATE_TRAINING;

		if (params->base.dramtype != LPDDR4) {
			ret = data_training(dram, channel, params,
					    training_flag);
			if (!ret) {
				debug("%s: data train failed for channel %d\n",
				      __func__, ret);
				continue;
			}
		}

		sdram_print_ddr_info(cap_info, &params->base);
		set_memory_map(chan, channel, params);
		cap_info->ddrconfig = calculate_ddrconfig(params, channel);

		set_ddrconfig(chan, params, channel, cap_info->ddrconfig);
		set_cap_relate_config(chan, params, channel);
	}

	if (params->base.num_channels == 0) {
		printf("%s: ", __func__);
		sdram_print_dram_type(params->base.dramtype);
		printf(" - %dMHz failed!\n", params->base.ddr_freq);
		return -EINVAL;
	}

	params->base.stride = calculate_stride(params);
	dram_all_config(dram, params);
	dram->ops->set_rate(dram, params);

	debug("Finish SDRAM initialization...\n");
	return 0;
}

static int rk3399_dmc_ofdata_to_platdata(struct udevice *dev)
{
#if !CONFIG_IS_ENABLED(OF_PLATDATA)
	struct rockchip_dmc_plat *plat = dev_get_platdata(dev);
	int ret;

	ret = dev_read_u32_array(dev, "rockchip,sdram-params",
				 (u32 *)&plat->sdram_params,
				 sizeof(plat->sdram_params) / sizeof(u32));
	if (ret) {
		printf("%s: Cannot read rockchip,sdram-params %d\n",
		       __func__, ret);
		return ret;
	}
	ret = regmap_init_mem(dev_ofnode(dev), &plat->map);
	if (ret)
		printf("%s: regmap failed %d\n", __func__, ret);

#endif
	return 0;
}

#if CONFIG_IS_ENABLED(OF_PLATDATA)
static int conv_of_platdata(struct udevice *dev)
{
	struct rockchip_dmc_plat *plat = dev_get_platdata(dev);
	struct dtd_rockchip_rk3399_dmc *dtplat = &plat->dtplat;
	int ret;

	ret = regmap_init_mem_platdata(dev, dtplat->reg,
				       ARRAY_SIZE(dtplat->reg) / 2,
				       &plat->map);
	if (ret)
		return ret;

	return 0;
}
#endif

static const struct sdram_rk3399_ops rk3399_ops = {
#if !defined(CONFIG_RAM_RK3399_LPDDR4)
	.data_training = default_data_training,
	.set_rate = switch_to_phy_index1,
#else
	.data_training = lpddr4_mr_detect,
	.set_rate = lpddr4_set_rate,
#endif
};

static int rk3399_dmc_init(struct udevice *dev)
{
	struct dram_info *priv = dev_get_priv(dev);
	struct rockchip_dmc_plat *plat = dev_get_platdata(dev);
	int ret;
#if !CONFIG_IS_ENABLED(OF_PLATDATA)
	struct rk3399_sdram_params *params = &plat->sdram_params;
#else
	struct dtd_rockchip_rk3399_dmc *dtplat = &plat->dtplat;
	struct rk3399_sdram_params *params =
					(void *)dtplat->rockchip_sdram_params;

	ret = conv_of_platdata(dev);
	if (ret)
		return ret;
#endif

	priv->ops = &rk3399_ops;
	priv->cic = syscon_get_first_range(ROCKCHIP_SYSCON_CIC);
	priv->grf = syscon_get_first_range(ROCKCHIP_SYSCON_GRF);
	priv->pmu = syscon_get_first_range(ROCKCHIP_SYSCON_PMU);
	priv->pmugrf = syscon_get_first_range(ROCKCHIP_SYSCON_PMUGRF);
	priv->pmusgrf = syscon_get_first_range(ROCKCHIP_SYSCON_PMUSGRF);
	priv->pmucru = rockchip_get_pmucru();
	priv->cru = rockchip_get_cru();
	priv->chan[0].pctl = regmap_get_range(plat->map, 0);
	priv->chan[0].pi = regmap_get_range(plat->map, 1);
	priv->chan[0].publ = regmap_get_range(plat->map, 2);
	priv->chan[0].msch = regmap_get_range(plat->map, 3);
	priv->chan[1].pctl = regmap_get_range(plat->map, 4);
	priv->chan[1].pi = regmap_get_range(plat->map, 5);
	priv->chan[1].publ = regmap_get_range(plat->map, 6);
	priv->chan[1].msch = regmap_get_range(plat->map, 7);

	debug("con reg %p %p %p %p %p %p %p %p\n",
	      priv->chan[0].pctl, priv->chan[0].pi,
	      priv->chan[0].publ, priv->chan[0].msch,
	      priv->chan[1].pctl, priv->chan[1].pi,
	      priv->chan[1].publ, priv->chan[1].msch);
	debug("cru %p, cic %p, grf %p, sgrf %p, pmucru %p, pmu %p\n", priv->cru,
	      priv->cic, priv->pmugrf, priv->pmusgrf, priv->pmucru, priv->pmu);

#if CONFIG_IS_ENABLED(OF_PLATDATA)
	ret = clk_get_by_index_platdata(dev, 0, dtplat->clocks, &priv->ddr_clk);
#else
	ret = clk_get_by_index(dev, 0, &priv->ddr_clk);
#endif
	if (ret) {
		printf("%s clk get failed %d\n", __func__, ret);
		return ret;
	}

	ret = clk_set_rate(&priv->ddr_clk, params->base.ddr_freq * MHz);
	if (ret < 0) {
		printf("%s clk set failed %d\n", __func__, ret);
		return ret;
	}

	ret = sdram_init(priv, params);
	if (ret < 0) {
		printf("%s DRAM init failed %d\n", __func__, ret);
		return ret;
	}

	return 0;
}
#endif

static int rk3399_dmc_probe(struct udevice *dev)
{
#if defined(CONFIG_TPL_BUILD) || \
	(!defined(CONFIG_TPL) && defined(CONFIG_SPL_BUILD))
	if (rk3399_dmc_init(dev))
		return 0;
#else
	struct dram_info *priv = dev_get_priv(dev);

	priv->pmugrf = syscon_get_first_range(ROCKCHIP_SYSCON_PMUGRF);
	debug("%s: pmugrf = %p\n", __func__, priv->pmugrf);
	priv->info.base = CONFIG_SYS_SDRAM_BASE;
	priv->info.size =
		rockchip_sdram_size((phys_addr_t)&priv->pmugrf->os_reg2);
#endif
	return 0;
}

static int rk3399_dmc_get_info(struct udevice *dev, struct ram_info *info)
{
	struct dram_info *priv = dev_get_priv(dev);

	*info = priv->info;

	return 0;
}

static struct ram_ops rk3399_dmc_ops = {
	.get_info = rk3399_dmc_get_info,
};

static const struct udevice_id rk3399_dmc_ids[] = {
	{ .compatible = "rockchip,rk3399-dmc" },
	{ }
};

U_BOOT_DRIVER(dmc_rk3399) = {
	.name = "rockchip_rk3399_dmc",
	.id = UCLASS_RAM,
	.of_match = rk3399_dmc_ids,
	.ops = &rk3399_dmc_ops,
#if defined(CONFIG_TPL_BUILD) || \
	(!defined(CONFIG_TPL) && defined(CONFIG_SPL_BUILD))
	.ofdata_to_platdata = rk3399_dmc_ofdata_to_platdata,
#endif
	.probe = rk3399_dmc_probe,
	.priv_auto_alloc_size = sizeof(struct dram_info),
#if defined(CONFIG_TPL_BUILD) || \
	(!defined(CONFIG_TPL) && defined(CONFIG_SPL_BUILD))
	.platdata_auto_alloc_size = sizeof(struct rockchip_dmc_plat),
#endif
};
