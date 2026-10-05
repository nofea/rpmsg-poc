/*
 * Bulk transfer protocol shared by the A53 app and the R5F firmware.
 *
 * RPMsg buffers hold at most 496 bytes of payload, so large payloads don't go
 * through the vrings. The sender writes the payload into its own area of the
 * bulk region and sends a struct rpmsg_bulk_msg (RPMSG_BULK_XFER) on the
 * "rpmsg-bulk" endpoint. The receiver reads the payload in place and answers
 * RPMSG_BULK_RELEASE with the same id, after which the sender may reuse its
 * area. One transfer per direction is in flight at a time.
 *
 * The bulk region must agree with renode/am64_zynqmp.repl,
 * zephyr_app/app.overlay and linux_app/am64_rpmsg_overlay.dts.
 */
#ifndef RPMSG_BULK_H
#define RPMSG_BULK_H

#include <stdint.h>

#define RPMSG_BULK_SERVICE_NAME "rpmsg-bulk"

#define RPMSG_BULK_REGION_ADDR 0xA8000000
#define RPMSG_BULK_REGION_SIZE 0x1000000

/* Each side only writes its own TX area */
#define RPMSG_BULK_A53_TX_OFFSET 0x000000
#define RPMSG_BULK_R5F_TX_OFFSET 0x800000
#define RPMSG_BULK_MAX_LEN       0x800000

#define RPMSG_BULK_XFER    1
#define RPMSG_BULK_RELEASE 2

/* Little-endian on the wire (both cores are little-endian) */
struct rpmsg_bulk_msg {
	uint32_t type;   /* RPMSG_BULK_XFER or RPMSG_BULK_RELEASE */
	uint32_t id;     /* transfer sequence number, echoed by RELEASE */
	uint32_t offset; /* payload offset within the bulk region */
	uint32_t len;    /* payload length in bytes */
	uint32_t crc32;  /* IEEE CRC32 of the payload (XFER only) */
};

#endif /* RPMSG_BULK_H */
