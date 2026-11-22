# SPDX-License-Identifier: GPL-2.0-only
obj-y := kirin710_tee.o

kirin710_tee-y := tc_client_driver.o smc_smp.o gp_ops.o mem.o agent.o \
		  mailbox_mempool.o security_auth_enhance.o dynamic_mem.o \
		  cmdmonitor.o tzdebug.o tz_spi_notify.o teek_client_api.o \
		  agent_rpmb.o tlogger.o tee_rdr_register.o secs_power_ctrl.o \
		  kirin710_atfd.o protected_mem.o protected_heap.o legacy_ion.o

kirin710_tee-y += libhwsecurec/memcpy_s.o libhwsecurec/memset_s.o \
		  libhwsecurec/strcpy_s.o libhwsecurec/strncpy_s.o \
		  libhwsecurec/memmove_s.o libhwsecurec/strcat_s.o \
		  libhwsecurec/strncat_s.o libhwsecurec/strtok_s.o \
		  libhwsecurec/securecutil.o libhwsecurec/secureprintoutput_a.o \
		  libhwsecurec/snprintf_s.o libhwsecurec/output.o \
		  libhwsecurec/vsnprintf_s.o

ccflags-y += -I$(src)/libhwsecurec \
	     -I$(src)/vendor-headers/include \
	     -I$(src)/vendor-headers/include/linux/mntn
ccflags-y += -DCONFIG_TEE_SMP -DCONFIG_TEE_CPU_MIGRATION -DCONFIG_TEE_CFC_ABI \
	     -DCONFIG_HISI_MMC_SECURE_RPMB -DCONFIG_DRMDRIVER

ccflags-y += -DCONFIG_TEELOG -DDYNAMIC_ION
