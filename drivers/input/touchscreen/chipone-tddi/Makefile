ccflags-y += -I$(srctree)/drivers/input/touchscreen/mediatek/
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/
ccflags-y += -I$(srctree)/drivers/misc/mediatek/include/mt-plat/$(MTK_PLATFORM)/include/
ccflags-y += -I$(srctree)/fs
ccflags-y += -I$(srctree)/drivers/misc/mediatek/hwmon/include


ccflags-$(CONFIG_DRM_MEDIATEK_V2) += -I$(srctree)/drivers/gpu/drm/mediatek/mediatek_v2/

obj-m += chipone-tddi.o

chipone-tddi-y += cts_tpd_driver.o cts_driver.o
chipone-tddi-y += cts_core.o
chipone-tddi-y += cts_sfctrlv2.o
chipone-tddi-y += cts_spi_flash.o
chipone-tddi-y += cts_firmware.o
chipone-tddi-y += cts_test.o
chipone-tddi-y += cts_charger_detect.o
chipone-tddi-y += cts_earjack_detect.o
chipone-tddi-y += cts_tcs.o
chipone-tddi-y += cts_platform.o
chipone-tddi-y += cts_tool.o
chipone-tddi-y += cts_sysfs.o
chipone-tddi-y += cts_strerror.o
chipone-tddi-y += cts_oem.o
chipone-tddi-y +=  tpd_control.o
chipone-tddi-y +=  tpd_button.o
chipone-tddi-y +=  tpd_debug.o
chipone-tddi-y +=  tpd_default.o
chipone-tddi-y +=  tpd_misc.o
chipone-tddi-y +=  tpd_setting.o
