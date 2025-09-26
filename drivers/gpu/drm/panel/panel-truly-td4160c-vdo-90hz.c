// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <drm/drm_modes.h>
#include <linux/delay.h>
#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <linux/backlight.h>
#include <linux/pinctrl/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>

#define CONFIG_MTK_PANEL_EXT
#if defined(CONFIG_MTK_PANEL_EXT)
#include "../mediatek/mediatek_v2/mtk_drm_graphics_base.h"
#include "../mediatek/mediatek_v2/mtk_log.h"
#include "../mediatek/mediatek_v2/mtk_panel_ext.h"
#endif

#if IS_ENABLED(CONFIG_PRIZE_HARDWARE_INFO)
#include "../../../misc/mediatek/prize/hardware_info/hardware_info.h"
extern struct hardware_info current_lcm_info;
#endif

#ifdef CONFIG_MTK_ROUND_CORNER_SUPPORT
#include "../mediatek/mtk_corner_pattern/mtk_data_hw_roundedpattern.h"
#endif

struct lcm {
	struct device *dev;
	struct drm_panel panel;
	struct backlight_device *backlight;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *bias_pos, *bias_neg;
	struct gpio_desc *pm_enable_gpio;

	bool prepared;
	bool enabled;

	int error;
};

struct i2c_client *lcm_i2c_client;

/* pri SL219A-744 add by zhanghuimin 20240313 start */
//extern int g_tp_gesture_flag;
/* pri SL219A-744 add by zhanghuimin 20240313 end */
extern unsigned int ilitek_tp_rst;

static int lcm_i2c_write_bytes(unsigned char addr, unsigned char value);

#define lcm_dcs_write_seq(ctx, seq...)                                         \
	({                                                                     \
		const u8 d[] = {seq};                                          \
		BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 64,                           \
				 "DCS sequence too big for stack");            \
		lcm_dcs_write(ctx, d, ARRAY_SIZE(d));                          \
	})

#define lcm_dcs_write_seq_static(ctx, seq...)                                  \
	({                                                                     \
		static const u8 d[] = {seq};                                   \
		lcm_dcs_write(ctx, d, ARRAY_SIZE(d));                          \
	})

static inline struct lcm *panel_to_lcm(struct drm_panel *panel)
{
	return container_of(panel, struct lcm, panel);
}

static void lcm_dcs_write(struct lcm *ctx, const void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;
	char *addr;

	if (ctx->error < 0)
		return;

	addr = (char *)data;
	if ((int)*addr < 0xB0)
		ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	else
		ret = mipi_dsi_generic_write(dsi, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %zd writing seq: %ph\n", ret, data);
		ctx->error = ret;
	}
}

#ifdef PANEL_SUPPORT_READBACK
static int lcm_dcs_read(struct lcm *ctx, u8 cmd, void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;

	if (ctx->error < 0)
		return 0;

	ret = mipi_dsi_dcs_read(dsi, cmd, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %d reading dcs seq:(%#x)\n", ret, cmd);
		ctx->error = ret;
	}

	return ret;
}

static void lcm_panel_get_data(struct lcm *ctx)
{
	u8 buffer[3] = {0};
	static int ret;

	if (ret == 0) {
		ret = lcm_dcs_read(ctx, 0x0A, buffer, 1);
		dev_info(ctx->dev, "return %d data(0x%08x) to dsi engine\n",
			 ret, buffer[0] | (buffer[1] << 8));
	}
}
#endif
static struct regulator *lcd_vdd_18;
static int lcm_panel_lcd_vdd_18_regulator_init(struct device *dev)
{
	int ret = 0;
	lcd_vdd_18 = regulator_get(dev, "lcd_vdd");
	if (IS_ERR_OR_NULL(lcd_vdd_18)) {
		ret = PTR_ERR(lcd_vdd_18);
		printk("[%s] get vdd regulator failed,ret=%d", __func__, ret);
		return ret;
	}
	
	if (regulator_count_voltages(lcd_vdd_18) > 0) {
		ret = regulator_set_voltage(lcd_vdd_18, 1800000, 1800000);
		if (ret) {
			printk("[%s]vdd regulator set_vtg failed ret=%d", __func__, ret);
			regulator_put(lcd_vdd_18);
			return ret;
		}
	}
	
	printk("%s OK!!!\n", __func__);
	return 0;
}
static void lcd_vdd_18_enable(void)
{
	int ret = 0;
	ret = regulator_enable(lcd_vdd_18);
	if (ret < 0)
		printk("[%s]regulator lcd_vdd_18 fail, ret = %d\n",  __func__,ret);
}
 
void lcd_vdd_18_disable(void)
{
	int ret = 0;
	ret = regulator_disable(lcd_vdd_18);
	if (ret < 0)
		printk("[%s]regulator lcd_vdd_18 fail, ret = %d\n",  __func__,ret);
}
static void lcm_panel_init(struct lcm *ctx)
{
	lcm_dcs_write_seq_static(ctx,0xB0, 0x80);
	lcm_dcs_write_seq_static(ctx,0xD6, 0x00);
	lcm_dcs_write_seq_static(ctx,0xB6, 0x30, 0x6A, 0x00, 0x06, 0xC3, 0x0b, 0xFF, 0xFF);
	lcm_dcs_write_seq_static(ctx,0xB7, 0x35, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx,0xB8, 0x57, 0x3D, 0x19, 0xBE, 0x1E, 0x0A);
	lcm_dcs_write_seq_static(ctx,0xB9, 0x6F, 0x3D, 0x28, 0xBE, 0x3C, 0x14);
	lcm_dcs_write_seq_static(ctx,0xBA, 0xB5, 0x33, 0x41, 0xBE, 0x64, 0x23);
	lcm_dcs_write_seq_static(ctx,0xBE, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx,0xC0, 0x00, 0x4a, 0x00, 0x24, 0x06, 0x4c, 0x00, 0x00, 0x16, 0x00, 0x10, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx,0xC1, 0x30, 0x11, 0x50, 0xFA, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x40, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xF5, 0x84, 0x7A, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx,0xC2, 0x02, 0x40, 0x24, 0x12, 0x04, 0x10, 0x08, 0x00, 0x00, 0x00, 0x02, 0x40, 0x24, 0x12, 0x08, 0x10, 0x0C, 0x00, 0x00, 0x00, 0x02, 0x40, 0x24, 0x12, 0x04, 0x10, 0x06, 0x00, 0x00, 0x00, 0x02, 0x40, 0x24, 0x12, 0x08, 0x10, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xE0, 0x3C, 0x10, 0x10, 0x04, 0x00, 0x81, 0x02, 0x10, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xE0, 0x3C, 0x10, 0x10, 0x08, 0x00, 0x81, 0x02, 0x10, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx,0xC3,0x02, 0x40, 0x24, 0x01, 0x01, 0x70, 0x1a, 0x00, 0x00, 0x00, 0x20, 0x00, 0x02, 0x40, 0x24, 0x01, 0x00, 0xF0, 0x16, 0x00, 0x00, 0x00, 0x20, 0x00, 0x02, 0x40, 0x24, 0x01, 0x01, 0x80, 0x1b, 0x00, 0x00, 0x00, 0x20, 0x00, 0x02, 0x40, 0x24, 0x01, 0x01, 0x00, 0x17, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x11, 0x00, 0x08, 0x00, 0x00, 0x00, 0x06, 0xb5, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0xb5, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx,0xC4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x08, 0x18, 0x10, 0x4E, 0x4C, 0x4A, 0x48, 0x46, 0x44, 0x42, 0x40, 0x82, 0x82, 0x82, 0x82, 0x41, 0x43, 0x45, 0x47, 0x49, 0x4B, 0x4D, 0x4F, 0x11, 0x19, 0x0A, 0x04, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x0F, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55);
	lcm_dcs_write_seq_static(ctx,0xC6, 0x00, 0x01, 0x00, 0x00, 0x01, 0x22, 0x04, 0x22, 0x01, 0x00, 0x30, 0x0B, 0x01, 0x35, 0xFF, 0x8F, 0x06, 0x15, 0x01, 0x00, 0x00, 0x00, 0x20, 0x20, 0x00, 0x00, 0x01, 0xC0, 0x11, 0x10, 0x00, 0x01, 0x00, 0x50, 0x00, 0x33, 0x83, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx,0xCA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE7, 0x11, 0x00, 0xC0, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx,0xCB, 0x02, 0xD0, 0x01, 0x80, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x04);
	lcm_dcs_write_seq_static(ctx,0xCE, 0x55, 0x40, 0x49, 0x53, 0x59, 0x5E, 0x63, 0x68, 0x6E, 0x74, 0x7E, 0x8A, 0x98, 0xA8, 0xBB, 0xD0, 0xE7, 0xFF, 0x04, 0x00, 0x04, 0x04, 0x42, 0x04, 0x04, 0x62, 0x40, 0x69, 0x5A, 0x00);
	lcm_dcs_write_seq_static(ctx,0xCF, 0x00);
	lcm_dcs_write_seq_static(ctx,0xD0, 0xD1, 0x50, 0x81, 0x66, 0x09, 0x90, 0x00, 0xC2, 0x12, 0x1F, 0x11, 0x32, 0x06, 0x7E, 0x09, 0x08, 0xC1, 0x1B, 0xF0, 0x06, 0x77, 0x05);
	lcm_dcs_write_seq_static(ctx,0xD1, 0xCA, 0xD6, 0x1b, 0x33, 0x33, 0x17, 0x07, 0xbb, 0x55, 0x55, 0x55, 0x55, 0x00, 0x33, 0x77, 0x07, 0x33, 0x30, 0x06, 0x72, 0x33, 0x13, 0x50, 0xD2, 0x1C, 0x55, 0x32, 0x00, 0x18, 0x70, 0x18, 0x77, 0x11, 0x11, 0x11, 0x20, 0x20, 0x30, 0x55);
	lcm_dcs_write_seq_static(ctx,0xD2, 0x00, 0x00, 0xA5);
	lcm_dcs_write_seq_static(ctx,0xD7, 0x21, 0x00, 0x12, 0x12, 0x00, 0x4a, 0x08, 0x58, 0x00, 0x4a, 0x08, 0x58, 0x00, 0x88, 0x88, 0x85, 0x85, 0x85, 0x87, 0x84, 0x05, 0x86, 0x87, 0x88, 0x88, 0x87, 0x89, 0x83, 0x88, 0x87, 0x84, 0x48, 0x90, 0x8C, 0x0B, 0x0A, 0xEA, 0xEA, 0xE7, 0xE7, 0x06, 0x06, 0x00, 0x08, 0x0A, 0x0A);
	lcm_dcs_write_seq_static(ctx,0xD8, 0x40, 0x99, 0x26, 0xED, 0x16, 0x6C, 0x16, 0x6C, 0x16, 0x6C, 0x00, 0x14, 0x00, 0x14, 0x00, 0x14, 0x01, 0x0C, 0x00, 0x00, 0x01, 0x00);
	lcm_dcs_write_seq_static(ctx,0xD9, 0x01, 0x02, 0x7F, 0x22, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x0C, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x0C, 0x00);
	lcm_dcs_write_seq_static(ctx,0xDD, 0x30, 0x06, 0x23, 0x65);
	lcm_dcs_write_seq_static(ctx,0xDE, 0x00, 0x00, 0x00, 0x0F, 0xFF, 0x00, 0x00, 0x00, 0x40, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50);
	//lcm_dcs_write_seq_static(ctx,0xDE, 0x01);
	lcm_dcs_write_seq_static(ctx,0xE6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx,0xE7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx,0xEB, 0x0b, 0x60, 0xb6, 0x00, 0x41, 0x01, 0x00, 0xCC, 0x0C, 0x08, 0x06, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	lcm_dcs_write_seq_static(ctx,0xEC, 0x60, 0x01);
	lcm_dcs_write_seq_static(ctx,0xED, 0x00, 0x00, 0xb6, 0x00, 0x02, 0x30, 0x00, 0x00, 0x00, 0x10, 0x00);
	lcm_dcs_write_seq_static(ctx,0xEE, 0x05, 0x40, 0x05, 0x00, 0xc0, 0x3F, 0x00, 0xc0, 0x3F, 0x00, 0xc0, 0x3F, 0x00, 0xc0, 0x3F, 0x00, 0xc0, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0xc0, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0xc0, 0x3F, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02);
	lcm_dcs_write_seq_static(ctx,0xB0, 0x83);



//## VCOM ADJ
	lcm_dcs_write_seq_static(ctx,0xB0, 0x84);
	lcm_dcs_write_seq_static(ctx,0xE5, 0x03, 0x3f, 0x00, 0x00);
//# 	lcm_dcs_write_seq_static(ctx,0xD5, 0x01, 0x45, 0x01, 0x45); vcom ������


//#Gamma setting
//#Gamma 2.2
	lcm_dcs_write_seq_static(ctx,0xC7,0x00,0x00,0x00,0x22,0x00,0x65,0x00,0xA7,0x01,0x24,0x01,0x49,0x01,0x47,0x01,0x47,0x01,0x44,0x01,0x49,0x01,0x4B,0x01,0x53,0x01,0x67,0x01,0x6D,0x01,0xA0,0x02,0x2F,0x02,0x55,0x02,0x67,0x02,0x6F,0x00,0x00,0x00,0x22,0x00,0x65,0x00,0xA7,0x01,0x24,0x01,0x49,0x01,0x47,0x01,0x47,0x01,0x44,0x01,0x49,0x01,0x4B,0x01,0x53,0x01,0x67,0x01,0x6D,0x01,0xA0,0x02,0x2F,0x02,0x55,0x02,0x67,0x02,0x6F);

//######## Display register setting ##########################################################################################################################

//#	lcm_dcs_write_seq_static(ctx,0xB0, 0x80);                                 ## Unlock Manufacture Command Access Protect of group M and A
//#	lcm_dcs_write_seq_static(ctx,0xD6, 0x00);                                 ## OTP/Flash Load setting (skip)
//#	lcm_dcs_write_seq_static(ctx,0xB0, 0x83);                                 ## Manufacture Command Access Protect

	lcm_dcs_write_seq_static(ctx,0x51, 0xFF);                                 //## Write_Display_Brightness
	lcm_dcs_write_seq_static(ctx,0x53, 0x0C);                                 //## Write_CTRL_Display
	lcm_dcs_write_seq_static(ctx,0x55, 0x00);                                 //## Write_CABC

	lcm_dcs_write_seq_static(ctx,0xB0, 0x83);
//#==========================================================
	lcm_dcs_write_seq_static(ctx,0x11,0x00);
	mdelay(80);
	lcm_dcs_write_seq_static(ctx,0x29,0x00);
	mdelay(10);
}

static int lcm_disable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = false;

	return 0;
}
/* pri SL219A-740 add by zhanghuimin 20240328 start */
struct pinctrl       *spi_mo_pinctrl;
struct pinctrl_state *spi_mo_pins_suspend;
struct pinctrl_state *spi_mo_pins_resume;

static int lcm_tp_spi_parse_dt(struct device *dev)
{
	int ret;

	pr_info("[%s] Enter !\n", __func__);

 	spi_mo_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(spi_mo_pinctrl)) {
		ret = PTR_ERR(spi_mo_pinctrl);
		pr_info("Cannot find touch pc!\n");
		goto err_out;
	}

	spi_mo_pins_suspend = pinctrl_lookup_state(spi_mo_pinctrl, "spi_cs_suspend");
	if (IS_ERR(spi_mo_pins_suspend)) {
		ret = PTR_ERR(spi_mo_pins_suspend);
		pr_info("Cannot find touch pinctrl spi_cs_suspend!");
		goto err_out;
	}

	spi_mo_pins_resume = pinctrl_lookup_state(spi_mo_pinctrl, "spi_cs_resume");
	if (IS_ERR(spi_mo_pins_resume)) {
		ret = PTR_ERR(spi_mo_pins_resume);
		pr_info("Cannot find touch pinctrl spi_cs_pins_resume!");
		goto err_out;
	}
	pr_info("[%s] Exit !\n", __func__);
	return 0;

err_out:
	pr_info("error out: %d", ret);
	pr_info("[%s] Exit2 !\n", __func__);
	return ret;
}

void lcm_tp_config_spi(int status)
{
	pr_info("[%s] Enter!\n", __func__);
	if (spi_mo_pinctrl == NULL || spi_mo_pins_suspend == NULL
                || spi_mo_pins_resume == NULL) {
		return;
	}
	if (status) {
		pinctrl_select_state(spi_mo_pinctrl, spi_mo_pins_suspend);
	} else {
		pinctrl_select_state(spi_mo_pinctrl, spi_mo_pins_resume);
	}

	pr_info("[%s]Exit!\n", __func__);
}
/* pri SL219A-744 add by zhanghuimin 20240313 end */
static int lcm_unprepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (!ctx->prepared)
		return 0;
	pr_info("%s\n", __func__);
	lcm_dcs_write_seq_static(ctx, 0x28);
	msleep(50);
	lcm_dcs_write_seq_static(ctx, 0x10);
	msleep(120);

	ctx->error = 0;
	ctx->prepared = false;
	/* pri SL219A-744 add by zhanghuimin 20240313 start */
		ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_LOW);
		gpiod_set_value(ctx->reset_gpio, 0);
		devm_gpiod_put(ctx->dev, ctx->reset_gpio);
		udelay(1000);
		/* pri add for SL005TC-296 20250701 start */
		gpio_set_value(ilitek_tp_rst, 0);
		lcm_tp_config_spi(1);
		/* pri SL219A-740 add by zhanghuimin 20240328 end */

		ctx->bias_neg =
		    devm_gpiod_get_index(ctx->dev, "bias", 0, GPIOD_OUT_HIGH);
		gpiod_set_value(ctx->bias_neg, 0);
		devm_gpiod_put(ctx->dev, ctx->bias_neg);
		udelay(10000);

		ctx->bias_pos =
		    devm_gpiod_get_index(ctx->dev, "bias", 1, GPIOD_OUT_HIGH);
		gpiod_set_value(ctx->bias_pos, 0);
		devm_gpiod_put(ctx->dev, ctx->bias_pos);
	/* pri SL219A-744 add by zhanghuimin 20240313 end */
	udelay(2000);

	lcd_vdd_18_disable();

	return 0;
}
/* pri SL219A-740 add by zhanghuimin 20240328 start */
static void lcm_reset(struct lcm *ctx)
{
	ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_LOW);
	gpiod_set_value(ctx->reset_gpio, 1);
	mdelay(5);
	gpiod_set_value(ctx->reset_gpio, 0);
	mdelay(15);
	gpiod_set_value(ctx->reset_gpio, 1);
	mdelay(20);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);
}
/* pri SL219A-740 add by zhanghuimin 20240328 end */
static int lcm_prepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	int ret;

	pr_info("%s\n", __func__);
	if (ctx->prepared)
		return 0;

	lcd_vdd_18_enable();
	udelay(2000);

	ctx->bias_pos =
	    devm_gpiod_get_index(ctx->dev, "bias", 1, GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->bias_pos, 1);
	devm_gpiod_put(ctx->dev, ctx->bias_pos);
	udelay(2000);

	ctx->bias_neg =
	    devm_gpiod_get_index(ctx->dev, "bias", 0, GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->bias_neg, 1);
	devm_gpiod_put(ctx->dev, ctx->bias_neg);
	udelay(2000);

	lcm_i2c_write_bytes(0x0, 0x14);
	lcm_i2c_write_bytes(0x1, 0x14);
	gpio_set_value(ilitek_tp_rst, 1);

	lcm_tp_config_spi(0);
	lcm_reset(ctx);
	udelay(1000);
	printk("[%s][%d] lcm_reset end\n", __func__, __LINE__);
	lcm_panel_init(ctx);
	printk("[%s][%d] lcm_panel_init end\n", __func__, __LINE__);
	ret = ctx->error;
	if (ret < 0)
		lcm_unprepare(panel);

	ctx->prepared = true;

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_rst(panel);
#endif
#ifdef PANEL_SUPPORT_READBACK
	lcm_panel_get_data(ctx);
#endif
	printk("[%s][%d] end\n", __func__, __LINE__);

	return ret;
}

static int lcm_enable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = true;

	return 0;
}
/* pri modify by zhanghuimin for SL219A-1321 20240407 start */
#define HFP (55)
/* pri modify by zhanghuimin for SL219A-1321 20240407 end */
#define HSA (4)
#define HBP (50)
#define VFP_90HZ (210)
#define VFP_60HZ (1140)
#define VSA (4)
#define VBP (33)
#define VAC (1612)
#define HAC (720)
#define PHYSICAL_WIDTH              67930
#define PHYSICAL_HEIGHT             152090

static struct drm_display_mode mode_90hz = {
	.clock = (HAC + HFP + HSA + HBP)*(VAC + VFP_90HZ + VSA + VBP)*90/1000,
	.hdisplay = HAC,
	.hsync_start = HAC + HFP,
	.hsync_end = HAC + HFP + HSA,
	.htotal = HAC + HFP + HSA + HBP,
	.vdisplay = VAC,
	.vsync_start = VAC + VFP_90HZ,
	.vsync_end = VAC + VFP_90HZ + VSA,
	.vtotal = VAC + VFP_90HZ + VSA + VBP,
};
/* pri add by zhanghuimin for SL219A-1298 20240407 start */
static struct drm_display_mode default_mode = {
	.clock = (HAC + HFP + HSA + HBP)*(VAC + VFP_60HZ + VSA + VBP)*60/1000,
	.hdisplay = HAC,
	.hsync_start = HAC + HFP,
	.hsync_end = HAC + HFP + HSA,
	.htotal = HAC + HFP + HSA + HBP,
	.vdisplay = VAC,
	.vsync_start = VAC + VFP_60HZ,
	.vsync_end = VAC + VFP_60HZ + VSA,
	.vtotal = VAC + VFP_60HZ + VSA + VBP,
};
/* pri add by zhanghuimin for SL219A-1298 20240407 end */
#if defined(CONFIG_MTK_PANEL_EXT)
static int panel_ext_reset(struct drm_panel *panel, int on)
{
	struct lcm *ctx = panel_to_lcm(panel);

	ctx->reset_gpio = devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	gpiod_set_value(ctx->reset_gpio, on);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	return 0;
}

static int panel_ata_check(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	unsigned char data[3] = {0x00, 0x00, 0x00};
	unsigned char id[3] = {0x41, 0x00, 0x00};
	ssize_t ret;

	ret = mipi_dsi_dcs_read(dsi, 0xda, data, 1);
	if (ret < 0) {
		pr_err("%s error\n", __func__);
		return 0;
	}

	pr_info("ATA read data %x %x %x\n", data[0], data[1], data[2]);

	if (data[0] == 0x41)
		return 1;

	pr_info("ATA expect read data is %x %x %x\n", id[0], id[1], id[2]);

	return 0;
}

static int lcm_setbacklight_cmdq(void *dsi, dcs_write_gce cb, void *handle,
				 unsigned int level)
{
	char bl_tb0[] = {0x51, 0xFF};

	bl_tb0[1] = level;

	if (!cb)
		return -1;

	cb(dsi, handle, bl_tb0, ARRAY_SIZE(bl_tb0));

	return 0;
}

static int lcm_get_virtual_heigh(void)
{
	return VAC;
}

static int lcm_get_virtual_width(void)
{
	return HAC;
}

static struct mtk_panel_params ext_params = {
/* pri add by zhanghuimin for SL219A-1321 20240407 start */
	.pll_clk = 456,
/* pri add by zhanghuimin for SL219A-1321 20240407 end */
//	.vfp_low_power = 840,
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_color_mode = MTK_DRM_COLOR_MODE_DISPLAY_P3,
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
/* pri modify by zhanghuimin for SL219A-1468 20240425 start */
	/*.lcm_esd_check_table[0] = {
		.cmd = 0x0b,
		.count = 1,
		.para_list[0] = 0x00,
	},*/
/* pri modify by zhanghuimin for SL219A-1468 20240425 end */
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
/* pri add by zhanghuimin for SL219A-1298 20240407 start */
	.dyn_fps = {
		.switch_en = 1, .vact_timing_fps = 90,
	},
};
static struct mtk_panel_params ext_params_60hz = {
	.pll_clk = 456,
//	.vfp_low_power = 840,
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_color_mode = MTK_DRM_COLOR_MODE_DISPLAY_P3,
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
/* pri modify by zhanghuimin for SL219A-1468 20240425 start */
	/*.lcm_esd_check_table[0] = {
		.cmd = 0x0b,
		.count = 1,
		.para_list[0] = 0x00,
	},*/
/* pri modify by zhanghuimin for SL219A-1468 20240425 end */
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.dyn_fps = {
		.switch_en = 1, .vact_timing_fps = 90,
	},
};
struct drm_display_mode *get_mode_by_id(struct drm_connector *connector,
	unsigned int mode)
{
	struct drm_display_mode *m;
	unsigned int i = 0;

	list_for_each_entry(m, &connector->modes, head) {
		if (i == mode)
			return m;
		i++;
	}
	return NULL;
}
static int mtk_panel_ext_param_set(struct drm_panel *panel,
			 struct drm_connector *connector, unsigned int mode)
{
	struct mtk_panel_ext *ext = find_panel_ext(panel);
	int ret = 0;
	struct drm_display_mode *m = get_mode_by_id(connector, mode);


	if (drm_mode_vrefresh(m) == 90)
		ext->params = &ext_params;
	else if (drm_mode_vrefresh(m) == 60)
		ext->params = &ext_params_60hz;
	else
		ret = 1;

	return ret;
}
/* pri add by zhanghuimin for SL219A-1298 20240407 end */
static struct mtk_panel_funcs ext_funcs = {
	.reset = panel_ext_reset,
	.set_backlight_cmdq = lcm_setbacklight_cmdq,
	.ata_check = panel_ata_check,
	.get_virtual_heigh = lcm_get_virtual_heigh,
	.get_virtual_width = lcm_get_virtual_width,
/* pri add by zhanghuimin for SL219A-1298 20240407 start */
	.ext_param_set = mtk_panel_ext_param_set,
/* pri add by zhanghuimin for SL219A-1298 20240407 end */
};
#endif

struct panel_desc {
	const struct drm_display_mode *modes;
	unsigned int num_modes;

	unsigned int bpc;

	struct {
		unsigned int width;
		unsigned int height;
	} size;

	struct {
		unsigned int prepare;
		unsigned int enable;
		unsigned int disable;
		unsigned int unprepare;
	} delay;
};
/* pri modify by zhanghuimin for SL219A-1298 20240407 start */
static int lcm_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	struct drm_display_mode *mode_1;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_info(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	mode_1 = drm_mode_duplicate(connector->dev, &mode_90hz);
	if (!mode_1) {
		dev_info(connector->dev->dev, "failed to add mode %ux%ux@%u\n",
			mode_90hz.hdisplay, mode_90hz.vdisplay,
			drm_mode_vrefresh(&mode_90hz));
		return -ENOMEM;
	}

	drm_mode_set_name(mode_1);
	mode_1->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_probed_add(connector, mode_1);

	connector->display_info.width_mm = 68;
	connector->display_info.height_mm = 152;

	return 2;
}
/* pri modify by zhanghuimin for SL219A-1298 20240407 end */
static const struct drm_panel_funcs lcm_drm_funcs = {
	.disable = lcm_disable,
	.unprepare = lcm_unprepare,
	.prepare = lcm_prepare,
	.enable = lcm_enable,
	.get_modes = lcm_get_modes,
};

static int lcm_i2c_write_bytes(unsigned char addr, unsigned char value)
{
	int ret = 0;
	struct i2c_client *client = lcm_i2c_client;
	char write_data[2] = { 0 };

	if (client == NULL) {
		pr_debug("ERROR!! lcm_i2c_client is null\n");
		return 0;
	}

	write_data[0] = addr;
	write_data[1] = value;
	ret = i2c_master_send(client, write_data, 2);
	if (ret < 0)
		pr_info("[LCM][ERROR] _lcm_i2c write data fail !!\n");

	return ret;
}

static int lcm_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	pr_info("[LCM][I2C]: info==>name=%s addr=0x%x\n", client->name,client->addr);
	lcm_i2c_client = client;
	return 0;
}

static int lcm_i2c_remove(struct i2c_client *client)
{
	pr_info("[LCM][I2C] %s\n", __func__);
	lcm_i2c_client = NULL;
	i2c_unregister_device(client);
	return 0;
}

static const struct of_device_id _lcm_i2c_of_match[] = {
	{
	    .compatible = "awinic,aw37501",
	},
	{},
};

static struct i2c_driver lcm_i2c_driver = {
	.probe = lcm_i2c_probe,
	.remove = lcm_i2c_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "aw37501",
		.of_match_table = _lcm_i2c_of_match,
	},
};

static int lcm_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct lcm *ctx;
	struct device_node *backlight;
	int ret;
	struct device_node *dsi_node, *remote_node = NULL, *endpoint = NULL;
	pr_info("%s+\n", __func__);
	dsi_node = of_get_parent(dev->of_node);
	if (dsi_node) {
		endpoint = of_graph_get_next_endpoint(dsi_node, NULL);
		if (endpoint) {
			remote_node = of_graph_get_remote_port_parent(endpoint);
			if (!remote_node) {
				pr_info("No panel connected,skip probe lcm\n");
				return -ENODEV;
			}
			pr_info("device node name:%s\n", remote_node->name);
		}
	}
	if (remote_node != dev->of_node) {
		pr_info("%s+ skip probe due to not current lcm\n", __func__);
		return -ENODEV;
	}

	ctx = devm_kzalloc(dev, sizeof(struct lcm), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_NO_EOT_PACKET;

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		ctx->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}

	i2c_add_driver(&lcm_i2c_driver);
	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpios %ld\n",
			PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	devm_gpiod_put(dev, ctx->reset_gpio);

	ctx->bias_pos = devm_gpiod_get_index(dev, "bias", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_pos)) {
		dev_err(dev, "cannot get bias-gpios 0 %ld\n",
			PTR_ERR(ctx->bias_pos));
		return PTR_ERR(ctx->bias_pos);
	}
	devm_gpiod_put(dev, ctx->bias_pos);

	ctx->bias_neg = devm_gpiod_get_index(dev, "bias", 1, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_neg)) {
		dev_err(dev, "cannot get bias-gpios 1 %ld\n",
			PTR_ERR(ctx->bias_neg));
		return PTR_ERR(ctx->bias_neg);
	}
	devm_gpiod_put(dev, ctx->bias_neg);

	lcm_panel_lcd_vdd_18_regulator_init(dev);
	lcd_vdd_18_enable();

#ifndef CONFIG_MTK_DISP_NO_LK
	ctx->prepared = true;
	ctx->enabled = true;
#endif

	drm_panel_init(&ctx->panel, dev, &lcm_drm_funcs, DRM_MODE_CONNECTOR_DSI);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &lcm_drm_funcs;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&ctx->panel);

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_handle_reg(&ctx->panel);
	ret = mtk_panel_ext_create(dev, &ext_params_60hz, &ext_funcs, &ctx->panel);
	if (ret < 0)
		return ret;
#endif

#if IS_ENABLED(CONFIG_PRIZE_HARDWARE_INFO)
	strcpy(current_lcm_info.chip,"TD4160C");
	strcpy(current_lcm_info.vendor,"TXD");
	sprintf(current_lcm_info.id,"0x41");
	strcpy(current_lcm_info.more,"720*1612");
#endif
/* pri SL219A-740 add by zhanghuimin 20240328 start */
	lcm_tp_spi_parse_dt(dev);
/* pri SL219A-740 add by zhanghuimin 20240328 end */
	pr_info("%s-\n", __func__);

	return ret;
}

static int lcm_remove(struct mipi_dsi_device *dsi)
{
	struct lcm *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
	return 0;

}

static const struct of_device_id lcm_of_match[] = {
	{ .compatible = "truly,td4160c,vdo", },
	{ }
};

MODULE_DEVICE_TABLE(of, lcm_of_match);

static struct mipi_dsi_driver lcm_driver = {
	.probe = lcm_probe,
	.remove = lcm_remove,
	.driver = {
		.name = "panel-truly-td4160c-vdo-90hz",
		.owner = THIS_MODULE,
		.of_match_table = lcm_of_match,
	},
};

module_mipi_dsi_driver(lcm_driver);

MODULE_AUTHOR("Tai-Hua Tseng <tai-hua.tseng@mediatek.com>");
MODULE_DESCRIPTION("xxx ili9883 VDO LCD Panel Driver");
MODULE_LICENSE("GPL v2");
