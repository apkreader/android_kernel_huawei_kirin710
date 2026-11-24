UBSAN_SANITIZE := n
KASAN_SANITIZE := n

# SPDX-License-Identifier: GPL-2.0-only
HI1102_ROOT := $(srctree)/drivers/net/wireless/hisilicon/hi1102
HI1102_DRIVER_SRC_PATH := $(HI1102_ROOT)/hisi
DRIVER_PATH := $(HI1102_DRIVER_SRC_PATH)/platform

export _PRE_PRODUCT_VERSION := 1102
export HI1102_BOARD_VERSION := v8r2
export RELEASE_TYPE := RELEASE

CONFIG_HI110X_KERNEL_MODULES_BUILD_SUPPORT := no

include $(DRIVER_PATH)/hi1102_comm_defconfig
include $(DRIVER_PATH)/plat_1102_v8r2_defconfig

oal-objs := oal_bus_if.o oal_main.o oal_mem.o oal_net.o oal_hardware.o \
            oal_schedule.o oal_pci_if.o oal_aes.o oal_kernel_file.o \
            oal_cfg80211.o oal_sdio_host.o oal_profiling.o oal_hcc_host.o \
            oal_hcc_test.o oal_softwdt.o oal_dft.o
oal-objs := $(addprefix oal/,$(oal-objs))

frw-objs := frw_event_deploy.o frw_event_main.o frw_event_sched.o \
            frw_ipc_msgqueue.o frw_main.o frw_task.o frw_timer.o
frw-objs := $(addprefix frw/,$(frw-objs))

oam-objs := oam_main.o oam_alarm.o oam_event.o oam_log.o oam_trace.o \
            oam_statistics.o oam_config.o oam_register.o oam_linux_netlink.o
oam-objs := $(addprefix oam/,$(oam-objs))

sdt-objs := sdt/sdt_drv.o
customize-objs := ../common/customize/hisi_ini.o ../common/customize/hisi_customize_wifi.o
chr_devs-objs := ../common/chr_log/chr_devs.o
main-objs := main/plat_main.o
pm-objs := pm/plat_firmware.o pm/plat_pm.o pm/plat_pm_wlan.o pm/plat_cali.o pm/plat_efuse.o
driver-objs := driver/plat_sdio.o driver/plat_uart.o
bfgx-objs := ../bfgx/bfgx_low_power.o ../bfgx/bfgx_exception_rst.o \
             ../bfgx/bfgx_data_parse.o ../bfgx/bfgx_dev.o ../bfgx/bfgx_user_ctrl.o
board-objs := board/board.o board/board-hi1102.o board/board-hi1103.o
oneimage-objs := ../common/oneimage/oneimage.o

plat-objs := $(oal-objs) $(frw-objs) $(oam-objs) $(sdt-objs) $(pm-objs) \
             $(main-objs) $(bfgx-objs) $(chr_devs-objs) $(board-objs) \
             $(oneimage-objs) $(customize-objs) $(driver-objs)

obj-y += plat.o

HI1102_INCLUDES := \
	-I$(HI1102_ROOT)/compat/include \
	-I$(DRIVER_PATH)/inc \
	-I$(DRIVER_PATH)/inc/frw \
	-I$(DRIVER_PATH)/inc/oal \
	-I$(DRIVER_PATH)/inc/oal/linux \
	-I$(DRIVER_PATH)/inc/oam \
	-I$(DRIVER_PATH)/inc/pm \
	-I$(DRIVER_PATH)/frw \
	-I$(DRIVER_PATH)/oal \
	-I$(DRIVER_PATH)/oam \
	-I$(DRIVER_PATH)/pm \
	-I$(DRIVER_PATH)/../bfgx \
	-I$(DRIVER_PATH)/sdt \
	-I$(DRIVER_PATH)/board \
	-I$(DRIVER_PATH)/driver \
	-I$(DRIVER_PATH)/../common/oneimage \
	-I$(DRIVER_PATH)/../common/customize \
	-I$(DRIVER_PATH)/../common/inc \
	-I$(DRIVER_PATH)/../common/chr_log \
	-I$(HI1102_DRIVER_SRC_PATH)/wifi/inc \
	-I$(HI1102_DRIVER_SRC_PATH)/wifi/inc/hal \
	-I$(HI1102_DRIVER_SRC_PATH)/wifi/inc/dmac \
	-I$(HI1102_DRIVER_SRC_PATH)/wifi/inc/wal

ccflags-y += -include $(HI1102_ROOT)/compat/hi1102_compat_612.h
ccflags-y += $(HI1102_INCLUDES)
ccflags-y += $(COPTS) $(COMM_COPTS)
ccflags-y += -DPLATFORM_TYPE_FOR_HI110X=0 -DPLATFORM_HI110X_k3v2oem1=1 -DPLATFORM_HI110X_UEDGE=2
ccflags-y += -DCONFIG_HI110X_SDIO_STD_CARD_SUPPORT -DCONFIG_HI110X_SDIO_DETECT_FUNCTION
ccflags-y += -DCONFIG_HI1102_PLAT_HW_CHR
ccflags-y += -Wno-error -Wno-unused-function -Wno-unused-variable -Wno-missing-prototypes \
             -Wno-implicit-fallthrough -Wno-misleading-indentation -Wno-format \
             -Wno-pointer-sign -Wno-switch -Wno-tautological-compare -Wno-empty-body \
             -Wno-enum-conversion -Wno-parentheses-equality
