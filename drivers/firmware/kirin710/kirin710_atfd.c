// SPDX-License-Identifier: GPL-2.0
#include <linux/arm-smccc.h>
#include <linux/hisi/hisi_drmdriver.h>

noinline s32 atfd_hisi_service_access_register_smc(u64 _main_fun_id,
						   u64 _buff_addr_phy,
						   u64 _data_len,
						   u64 _sub_fun_id)
{
	struct arm_smccc_res result;

	arm_smccc_smc(_main_fun_id, _buff_addr_phy, _data_len, _sub_fun_id,
		      0, 0, 0, 0, &result);
	return (s32)result.a0;
}
