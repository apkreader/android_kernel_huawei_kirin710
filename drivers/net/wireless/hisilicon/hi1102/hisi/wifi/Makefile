UBSAN_SANITIZE := n
KASAN_SANITIZE := n

# SPDX-License-Identifier: GPL-2.0-only
HI1102_ROOT := $(srctree)/drivers/net/wireless/hisilicon/hi1102
HI1102_DRIVER_SRC_PATH := $(HI1102_ROOT)/hisi
DRIVER_PATH := $(HI1102_DRIVER_SRC_PATH)/wifi

export _PRE_PRODUCT_VERSION := 1102
export HI1102_BOARD_VERSION := v8r2
export RELEASE_TYPE := RELEASE

CONFIG_HI110X_KERNEL_MODULES_BUILD_SUPPORT := no

include $(HI1102_DRIVER_SRC_PATH)/platform/hi1102_comm_defconfig
include $(DRIVER_PATH)/wifi_1102_v8r2_defconfig

hmac-objs := hmac_main.o hmac_rx_data.o hmac_tx_amsdu.o hmac_tx_data.o \
             hmac_user.o hmac_vap.o hmac_mgmt_bss_comm.o hmac_mgmt_classifier.o \
             hmac_fsm.o hmac_mgmt_ap.o hmac_mgmt_sta.o hmac_encap_frame_ap.o \
             hmac_encap_frame_sta.o hmac_encap_frame.o hmac_sme_sta.o hmac_11i.o \
             hmac_uapsd.o hmac_config.o hmac_protection.o hmac_frag.o \
             hmac_crypto_tkip.o hmac_chan_mgmt.o hmac_m2u.o hmac_smps.o \
             hmac_data_acq.o hmac_rx_filter.o hmac_proxy_arp.o hmac_hcc_adapt.o \
             hmac_reset.o hmac_scan.o hmac_p2p.o hmac_device.o hmac_resource.o \
             hmac_wapi_wpi.o hmac_wapi_sms4.o hmac_wapi.o hmac_cali_mgmt.o \
             hmac_cali_dpd.o hmac_custom_security.o hmac_blockack.o hmac_dfs.o \
             hmac_psm_ap.o hmac_dfx.o hmac_edca_opt.o hmac_arp_offload.o \
             hmac_verify/hmac_test_main.o hmac_tcp_opt.o hmac_auto_adjust_freq.o \
             hmac_roam_alg.o hmac_roam_connect.o hmac_roam_main.o \
             hmac_traffic_classify.o hmac_proxysta.o hmac_wmmac.o hmac_btcoex.o
hmac-objs := $(addprefix hmac/,$(hmac-objs))

mac-objs := mac_device.o mac_frame.o mac_user.o mac_vap.o mac_regdomain.o \
            mac_ie.o mac_resource.o mac_board.o mac_pm.o mac_data.o
mac-objs := $(addprefix dmac/,$(mac-objs))

wal-objs := wal_config.o wal_config_debug.o wal_linux_bridge.o wal_main.o \
            wal_linux_scan.o wal_linux_rx_rsp.o wal_linux_event.o wal_regdb.o \
            wal_config_acs.o wal_linux_flowctl.o wal_linux_ioctl.o \
            wal_linux_ioctl_debug.o wal_linux_atcmdsrv.o wal_linux_cfg80211.o \
            wal_dfx.o wal_linux_cfgvendor.o
wal-objs := $(addprefix wal/,$(wal-objs))

wifi-objs := $(wal-objs) main/main.o $(hmac-objs) $(mac-objs)
obj-y += wifi.o

HI1102_INCLUDES := \
	-I$(HI1102_ROOT)/compat/include \
	-I$(DRIVER_PATH)/inc \
	-I$(HI1102_DRIVER_SRC_PATH)/platform/inc \
	-I$(HI1102_DRIVER_SRC_PATH)/platform/board \
	-I$(DRIVER_PATH)/inc/hmac \
	-I$(DRIVER_PATH)/inc/wal \
	-I$(DRIVER_PATH)/hmac \
	-I$(DRIVER_PATH)/wal \
	-I$(HI1102_DRIVER_SRC_PATH)/platform/inc/frw \
	-I$(HI1102_DRIVER_SRC_PATH)/platform/inc/oal \
	-I$(HI1102_DRIVER_SRC_PATH)/platform/inc/oal/linux \
	-I$(HI1102_DRIVER_SRC_PATH)/platform/inc/pm \
	-I$(HI1102_DRIVER_SRC_PATH)/platform/inc/oam \
	-I$(HI1102_DRIVER_SRC_PATH)/platform/oam \
	-I$(HI1102_DRIVER_SRC_PATH)/platform/pm \
	-I$(HI1102_DRIVER_SRC_PATH)/platform/oal \
	-I$(HI1102_DRIVER_SRC_PATH)/platform/frw \
	-I$(HI1102_DRIVER_SRC_PATH)/platform/sdt \
	-I$(HI1102_DRIVER_SRC_PATH)/common/customize \
	-I$(HI1102_DRIVER_SRC_PATH)/common/oneimage \
	-I$(HI1102_DRIVER_SRC_PATH)/common/inc \
	-I$(HI1102_DRIVER_SRC_PATH)/common/chr_log \
	-I$(DRIVER_PATH)/inc/dmac \
	-I$(DRIVER_PATH)/inc/hal \
	-I$(DRIVER_PATH)/hal \
	-I$(DRIVER_PATH)/hal/hal_verify \
	-I$(DRIVER_PATH)/hmac/hmac_verify \
	-I$(DRIVER_PATH)/dmac/dmac_verify \
	-I$(DRIVER_PATH)/alg/alg_verify \
	-I$(DRIVER_PATH)/dmac \
	-I$(DRIVER_PATH)/alg

ccflags-y += -include $(HI1102_ROOT)/compat/hi1102_compat_612.h
ccflags-y += $(HI1102_INCLUDES)
ccflags-y += $(COPTS) $(COMM_COPTS)
ccflags-y += -DCONFIG_HI110X_SDIO_STD_CARD_SUPPORT -DCONFIG_HI110X_SDIO_DETECT_FUNCTION
ccflags-y += -D_PRE_WLAN_FEATURE_SAE
ccflags-y += -Wno-error -Wno-unused-function -Wno-unused-variable -Wno-missing-prototypes \
             -Wno-implicit-fallthrough -Wno-misleading-indentation -Wno-format \
             -Wno-pointer-sign -Wno-switch -Wno-tautological-compare -Wno-empty-body \
             -Wno-enum-conversion -Wno-parentheses-equality
