#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include "rsc_table.h"

#define RSC_TABLE_NODE DT_CHOSEN(zephyr_ipc_rsc_table)
#define SHM_NODE       DT_CHOSEN(zephyr_ipc_shm)

/* VRING0 and VRING1 sit at the start of the shared SRAM, 16 KB apart */
#define VRING0_ADDR (DT_REG_ADDR(SHM_NODE))
#define VRING1_ADDR (DT_REG_ADDR(SHM_NODE) + 0x4000)

BUILD_ASSERT(sizeof(struct am64_rsc_table) <= DT_REG_SIZE(RSC_TABLE_NODE),
	     "resource table does not fit in zephyr,ipc_rsc_table");

/*
 * Nothing loads this firmware through remoteproc, so the A53 cannot read the
 * table from the ELF. Instead it is copied at boot to a fixed address that the
 * A53 maps through UIO.
 */
static const struct am64_rsc_table rsc_table_template = {
	.hdr = {
		.ver = 1,
		.num = ARRAY_SIZE(rsc_table_template.offset),
	},
	.offset = { offsetof(struct am64_rsc_table, vdev) },
	.vdev = {
		.type = RSC_VDEV,
		.id = VIRTIO_ID_RPMSG,
		.notifyid = VDEV_NOTIFYID,
		.dfeatures = BIT(VIRTIO_RPMSG_F_NS),
		.gfeatures = 0,
		.config_len = 0,
		.status = 0,
		.num_of_vrings = VRING_COUNT,
	},
	.vring0 = {
		.da = VRING0_ADDR,
		.align = VRING_ALIGN,
		.num = VRING_NUM_DESCS,
		.notifyid = VRING0_NOTIFYID,
	},
	.vring1 = {
		.da = VRING1_ADDR,
		.align = VRING_ALIGN,
		.num = VRING_NUM_DESCS,
		.notifyid = VRING1_NOTIFYID,
	},
};

struct am64_rsc_table *rsc_table_publish(void)
{
	struct am64_rsc_table *table = (struct am64_rsc_table *)DT_REG_ADDR(RSC_TABLE_NODE);

	memcpy(table, &rsc_table_template, sizeof(*table));

	return table;
}
