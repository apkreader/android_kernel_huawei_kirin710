#ifndef _DYNAMIC_MMEM_H_
#define _DYNAMIC_MMEM_H_
#include "protected_mem.h"

#include "teek_ns_client.h"
#define CAFD_MAX         10 //concurrent opened session count
#define SET_BIT(map, bit) (map |= (0x1<<(bit)))
#define CLR_BIT(map, bit) (map &= (~(unsigned)(0x1<<(bit))))
struct dynamic_mem_item{
	struct list_head head;
	uint32_t configid;
	uint32_t size;
	struct kirin710_sync_buffer memory;
	bool unregistered;
	bool registration_failed;
	uint32_t cafd[CAFD_MAX];
	uint32_t cafd_count_bitmap;
	uint32_t cafd_count;
	TEEC_UUID uuid;
};
int init_dynamic_mem(void);
void exit_dynamic_mem(void);
int load_app_use_configid(uint32_t configid, uint32_t cafd,  TEEC_UUID* uuid, uint32_t size);
void kill_ion_by_cafd(unsigned int cafd);
void kill_ion_by_uuid(TEEC_UUID* uuid);
int add_cafd_count_by_uuid(TEEC_UUID* uuid, uint32_t cafd);
int is_used_dynamic_mem(TEEC_UUID *uuid);
#endif
