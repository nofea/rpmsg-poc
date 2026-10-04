#ifndef RSC_TABLE_H_
#define RSC_TABLE_H_

#include <openamp/remoteproc.h>
#include <openamp/rpmsg_virtio.h>
#include <openamp/virtio.h>

#define VRING_COUNT     2
#define VRING_NUM_DESCS 256
#define VRING_ALIGN     0x1000

/* Notification IDs; both sides process all vrings on every kick */
#define VRING0_NOTIFYID 0
#define VRING1_NOTIFYID 1
#define VDEV_NOTIFYID   2

struct am64_rsc_table {
	struct resource_table hdr;
	uint32_t offset[1];
	struct fw_rsc_vdev vdev;
	struct fw_rsc_vdev_vring vring0;
	struct fw_rsc_vdev_vring vring1;
} METAL_PACKED_END;

/*
 * Publish the resource table at the address shared with the A53
 * (chosen zephyr,ipc_rsc_table) and return a pointer to that copy.
 */
struct am64_rsc_table *rsc_table_publish(void);

#endif /* RSC_TABLE_H_ */
