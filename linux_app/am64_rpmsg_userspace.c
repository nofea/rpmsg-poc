/*
 * A53 (Linux) side of the AM64x RPMsg POC: an OpenAMP RPMsg host (virtio
 * driver) in userspace. libmetal maps the resource table, the shared SRAM and
 * the IPI registers through UIO (see am64_rpmsg_overlay.dts).
 *
 * Flow: wait for the R5F to publish its resource table, initialize the vrings
 * and buffers (sets DRIVER_OK), wait for the R5F's name service announcement
 * and send a zero-length message so the R5F learns our endpoint address. This
 * is done for both endpoints: "rpmsg-client-sample" carries text messages,
 * "rpmsg-bulk" carries descriptors of payloads in the bulk region (see
 * common/rpmsg_bulk.h). Then:
 *
 *   am64_rpmsg_userspace <arg> [<arg> ...]   send each argument as a message,
 *                                            or "--bulk <file>" as a bulk
 *                                            payload, print replies until idle
 *                                            for 2 s
 *   am64_rpmsg_userspace                     interactive: send each stdin line
 *                                            ("/bulk <file>" sends a file as a
 *                                            bulk payload), print incoming
 *                                            messages, exit on "quit" or EOF
 *
 * Bulk payloads received from the R5F are saved to /tmp/bulk_<id>.bin.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <metal/device.h>
#include <metal/io.h>
#include <metal/irq.h>
#include <metal/sys.h>
#include <openamp/open_amp.h>
#include <openamp/remoteproc.h>
#include "rpmsg_bulk.h"

#define BUS_NAME     "platform"
#define RSC_DEV_NAME "a0100000.rsc_table"
#define SHM_DEV_NAME "a5000000.shm"
#define IPI_DEV_NAME "ff340000.ipi"
#define BULK_DEV_NAME "a8000000.bulk"

/* ZynqMP IPI registers of the APU's channel 7 */
#define IPI_TRIG_OFFSET 0x00
#define IPI_ISR_OFFSET  0x10
#define IPI_IER_OFFSET  0x18
#define IPI_IDR_OFFSET  0x1c
#define IPI_RPU0_MASK   0x100 /* RPU0 is IPI channel 1 (bit 8) */

/* RPMsg buffers follow VRING0 (+0x0) and VRING1 (+0x4000) in the shared SRAM */
#define SHM_BUF_OFFSET 0x8000

#define RPMSG_SERVICE_NAME "rpmsg-client-sample"

/* 512-byte RPMsg buffer minus the 16-byte RPMsg header */
#define MAX_PAYLOAD 496

#define RSC_TABLE_TIMEOUT_S 30
#define NS_TIMEOUT_S        60
#define CLI_IDLE_MS         2000
#define POLL_INTERVAL_MS    1
#define LINE_BUF_SIZE       1024
#define BULK_RELEASE_TIMEOUT_S 60
#define BULK_CHUNK_SIZE     65536
#define BULK_SAVE_FMT       "/tmp/bulk_%u.bin"

#define LOG(fmt, ...) printf("am64_rpmsg_userspace: " fmt "\n", ##__VA_ARGS__)

struct platform {
	struct metal_device *rsc_dev, *shm_dev, *ipi_dev, *bulk_dev;
	struct metal_io_region *rsc_io, *shm_io, *ipi_io, *bulk_io;
	struct remoteproc_mem rsc_mem, shm_mem;
	atomic_int kicked;
};

static struct platform plat;
static struct remoteproc rproc;
static struct rpmsg_virtio_device rvdev;
static struct rpmsg_virtio_shm_pool shpool;
static struct rpmsg_endpoint ept, bulk_ept;
static int ept_bound, bulk_ept_bound;
static long last_activity_ms;

/* Set while our A53->R5F area holds a payload the R5F hasn't released */
static int bulk_in_flight;
static uint32_t bulk_next_id = 1;

static int ipi_irq_handler(int vector, void *priv)
{
	struct platform *p = priv;

	(void)vector;
	if (!(metal_io_read32(p->ipi_io, IPI_ISR_OFFSET) & IPI_RPU0_MASK))
		return METAL_IRQ_NOT_HANDLED;

	metal_io_write32(p->ipi_io, IPI_ISR_OFFSET, IPI_RPU0_MASK);
	atomic_store(&p->kicked, 1);
	return METAL_IRQ_HANDLED;
}

static struct remoteproc *rproc_init(struct remoteproc *rproc,
				     const struct remoteproc_ops *ops, void *arg)
{
	(void)ops;
	(void)arg;
	return rproc;
}

/* Kick the R5F */
static int rproc_notify(struct remoteproc *rproc, uint32_t id)
{
	struct platform *p = rproc->priv;

	(void)id;
	metal_io_write32(p->ipi_io, IPI_TRIG_OFFSET, IPI_RPU0_MASK);
	return 0;
}

static const struct remoteproc_ops rproc_ops = {
	.init = rproc_init,
	.notify = rproc_notify,
};

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int endpoint_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
		       uint32_t src, void *priv)
{
	(void)ept;
	(void)src;
	(void)priv;
	LOG("received \"%.*s\"", (int)len, (char *)data);
	last_activity_ms = now_ms();
	return RPMSG_SUCCESS;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
	static uint32_t table[256];
	size_t i;

	if (!table[1]) {
		for (i = 0; i < 256; i++) {
			uint32_t c = i;
			int k;

			for (k = 0; k < 8; k++)
				c = (c >> 1) ^ (c & 1 ? 0xEDB88320 : 0);
			table[i] = c;
		}
	}

	crc = ~crc;
	for (i = 0; i < len; i++)
		crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static int bulk_send_ctrl(uint32_t type, uint32_t id, uint32_t offset, uint32_t len,
			  uint32_t crc)
{
	struct rpmsg_bulk_msg msg = {
		.type = type, .id = id, .offset = offset, .len = len, .crc32 = crc,
	};

	return rpmsg_send(&bulk_ept, &msg, sizeof(msg));
}

/*
 * Compute the CRC of a payload in the bulk region and, if fd >= 0, copy it to
 * fd. Clears *fd on a write error. The bulk region is mapped uncached (device
 * memory), so it is only accessed through metal_io_block_read/write, which
 * keep the accesses aligned.
 */
static uint32_t bulk_read(unsigned long offset, size_t len, int *fd)
{
	static uint8_t chunk[BULK_CHUNK_SIZE];
	uint32_t crc = 0;

	while (len) {
		size_t n = len < sizeof(chunk) ? len : sizeof(chunk);

		metal_io_block_read(plat.bulk_io, offset, chunk, n);
		crc = crc32_update(crc, chunk, n);
		if (*fd >= 0 && write(*fd, chunk, n) != (ssize_t)n) {
			LOG("failed to save bulk payload: %s", strerror(errno));
			close(*fd);
			*fd = -1;
		}
		offset += n;
		len -= n;
	}
	return crc;
}

/* The R5F wrote a payload into its area: check it in place, then release it */
static int bulk_endpoint_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
			    uint32_t src, void *priv)
{
	struct rpmsg_bulk_msg msg;
	char path[64];
	uint32_t crc;
	int fd;

	(void)ept;
	(void)src;
	(void)priv;
	if (len != sizeof(msg)) {
		LOG("dropped bulk message with bad length %zu", len);
		return RPMSG_SUCCESS;
	}
	memcpy(&msg, data, sizeof(msg));
	last_activity_ms = now_ms();

	switch (msg.type) {
	case RPMSG_BULK_XFER:
		if (msg.offset < RPMSG_BULK_R5F_TX_OFFSET || msg.len > RPMSG_BULK_MAX_LEN ||
		    msg.offset - RPMSG_BULK_R5F_TX_OFFSET > RPMSG_BULK_MAX_LEN - msg.len) {
			LOG("dropped bulk %u outside the R5F area (offset 0x%x, len %u)",
			    msg.id, msg.offset, msg.len);
			return RPMSG_SUCCESS;
		}
		snprintf(path, sizeof(path), BULK_SAVE_FMT, msg.id);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			LOG("failed to create %s: %s", path, strerror(errno));
		crc = bulk_read(msg.offset, msg.len, &fd);
		LOG("received bulk %u: %u bytes, crc 0x%08x %s", msg.id, msg.len, crc,
		    crc == msg.crc32 ? "OK" : "MISMATCH");
		if (fd >= 0) {
			close(fd);
			LOG("saved bulk %u to %s", msg.id, path);
		}
		if (bulk_send_ctrl(RPMSG_BULK_RELEASE, msg.id, 0, 0, 0) < 0)
			LOG("failed to release bulk %u", msg.id);
		break;
	case RPMSG_BULK_RELEASE:
		bulk_in_flight = 0;
		LOG("bulk %u released", msg.id);
		break;
	default:
		LOG("dropped bulk message with unknown type %u", msg.type);
		break;
	}
	return RPMSG_SUCCESS;
}

static void ns_bind_cb(struct rpmsg_device *rdev, const char *name, uint32_t dest)
{
	struct rpmsg_endpoint *e;
	rpmsg_ept_cb cb;
	int *bound;

	LOG("name service announcement: \"%s\" at address 0x%x", name, dest);
	if (!strcmp(name, RPMSG_SERVICE_NAME)) {
		e = &ept;
		cb = endpoint_cb;
		bound = &ept_bound;
	} else if (!strcmp(name, RPMSG_BULK_SERVICE_NAME)) {
		e = &bulk_ept;
		cb = bulk_endpoint_cb;
		bound = &bulk_ept_bound;
	} else {
		return;
	}

	if (rpmsg_create_ept(e, rdev, name, RPMSG_ADDR_ANY, dest, cb, NULL)) {
		LOG("failed to create endpoint \"%s\"", name);
		return;
	}
	*bound = 1;
}

static int open_uio(const char *name, struct metal_device **dev, struct metal_io_region **io)
{
	int ret = metal_device_open(BUS_NAME, name, dev);

	if (ret) {
		LOG("failed to open UIO device %s: %d", name, ret);
		return ret;
	}
	*io = metal_device_io_region(*dev, 0);
	if (!*io) {
		LOG("UIO device %s has no I/O region", name);
		return -1;
	}
	LOG("opened %s (pa 0x%lx, size 0x%zx)", name,
	    (unsigned long)metal_io_phys(*io, 0), metal_io_region_size(*io));
	return 0;
}

static int add_mem(struct remoteproc_mem *mem, const char *name, struct metal_io_region *io)
{
	metal_phys_addr_t pa = metal_io_phys(io, 0);

	/* The R5F sees the same physical addresses (da == pa) */
	remoteproc_init_mem(mem, name, pa, pa, metal_io_region_size(io), io);
	remoteproc_add_mem(&rproc, mem);
	return 0;
}

static int wait_for_rsc_table(void)
{
	int i;

	for (i = 0; i < RSC_TABLE_TIMEOUT_S * 10; i++) {
		if (metal_io_read32(plat.rsc_io, offsetof(struct resource_table, ver)) == 1 &&
		    metal_io_read32(plat.rsc_io, offsetof(struct resource_table, num)) > 0)
			return 0;
		usleep(100000);
	}
	return -1;
}

/*
 * Wait up to POLL_INTERVAL_MS for input on fd (none if fd < 0), and let OpenAMP
 * process the vrings if the R5F kicked us. Returns nonzero if fd is readable.
 */
static int wait_for_events(int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int ready = 0;

	if (fd >= 0)
		ready = poll(&pfd, 1, POLL_INTERVAL_MS) > 0;
	else
		usleep(POLL_INTERVAL_MS * 1000);

	if (atomic_exchange(&plat.kicked, 0))
		remoteproc_get_notification(&rproc, RSC_NOTIFY_ID_ANY);
	return ready;
}

static int send_msg(const char *text, size_t len)
{
	/* Zero-length messages are reserved for the connect notification */
	if (len == 0) {
		LOG("empty messages can't be sent");
		return -1;
	}
	if (len > MAX_PAYLOAD) {
		LOG("message too long (%zu bytes, max %d)", len, MAX_PAYLOAD);
		return -1;
	}
	if (rpmsg_send(&ept, text, len) < 0) {
		LOG("rpmsg_send failed");
		return -1;
	}
	LOG("sent \"%.*s\"", (int)len, text);
	last_activity_ms = now_ms();
	return 0;
}

/* Process kicks until the R5F has released our last bulk payload */
static int wait_bulk_released(void)
{
	long deadline = now_ms() + BULK_RELEASE_TIMEOUT_S * 1000L;

	while (bulk_in_flight && now_ms() < deadline)
		wait_for_events(-1);
	if (bulk_in_flight) {
		LOG("timed out waiting for the R5F to release the bulk payload");
		return -1;
	}
	return 0;
}

/* Copy a file into our A53->R5F area and hand it to the R5F */
static int send_bulk(const char *path)
{
	static uint8_t chunk[BULK_CHUNK_SIZE];
	size_t len = 0;
	uint32_t crc = 0, id;
	ssize_t n;
	int fd;

	if (bulk_in_flight) {
		LOG("previous bulk transfer still in flight");
		return -1;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		LOG("failed to open %s: %s", path, strerror(errno));
		return -1;
	}
	while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
		if (len + n > RPMSG_BULK_MAX_LEN) {
			LOG("%s is too large (max %d bytes)", path, RPMSG_BULK_MAX_LEN);
			close(fd);
			return -1;
		}
		metal_io_block_write(plat.bulk_io, RPMSG_BULK_A53_TX_OFFSET + len, chunk, n);
		crc = crc32_update(crc, chunk, n);
		len += n;
	}
	close(fd);
	if (n < 0) {
		LOG("failed to read %s: %s", path, strerror(errno));
		return -1;
	}
	if (len == 0) {
		LOG("%s is empty", path);
		return -1;
	}

	/* The payload must be visible before the R5F sees the descriptor */
	atomic_thread_fence(memory_order_seq_cst);
	id = bulk_next_id++;
	bulk_in_flight = 1;
	if (bulk_send_ctrl(RPMSG_BULK_XFER, id, RPMSG_BULK_A53_TX_OFFSET, len, crc) < 0) {
		bulk_in_flight = 0;
		LOG("failed to send bulk descriptor");
		return -1;
	}
	LOG("sent bulk %u: %zu bytes, crc 0x%08x", id, len, crc);
	last_activity_ms = now_ms();
	return 0;
}

/*
 * Send each argument ("--bulk <file>" as a bulk payload), then print incoming
 * messages until idle for CLI_IDLE_MS
 */
static int run_cli(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--bulk")) {
			if (++i == argc) {
				LOG("--bulk needs a file argument");
				return -1;
			}
			if (send_bulk(argv[i]) || wait_bulk_released())
				return -1;
		} else if (send_msg(argv[i], strlen(argv[i]))) {
			return -1;
		}
	}

	while (now_ms() - last_activity_ms < CLI_IDLE_MS)
		wait_for_events(-1);
	return 0;
}

/* Returns 1 on "quit", 0 otherwise */
static int handle_line(const char *line, size_t len)
{
	if (len && line[len - 1] == '\r')
		len--;
	if (len == 0)
		return 0;
	if (len == 4 && !memcmp(line, "quit", 4))
		return 1;
	if (len > 6 && !memcmp(line, "/bulk ", 6)) {
		char path[LINE_BUF_SIZE];

		memcpy(path, line + 6, len - 6);
		path[len - 6] = '\0';
		send_bulk(path);
		return 0;
	}

	send_msg(line, len);
	return 0;
}

/* Send each stdin line, print incoming messages as they arrive */
static int run_interactive(void)
{
	char buf[LINE_BUF_SIZE];
	size_t used = 0;
	int discarding = 0;

	LOG("interactive mode: type a message and press Enter, \"quit\" or Ctrl-D to exit");
	for (;;) {
		char *start = buf, *nl;
		ssize_t n;

		if (!wait_for_events(STDIN_FILENO))
			continue;

		n = read(STDIN_FILENO, buf + used, sizeof(buf) - used);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			LOG("failed to read stdin: %s", strerror(errno));
			return -1;
		}
		if (n == 0) {
			/* EOF: send an unterminated last line, if any */
			if (used && !discarding)
				handle_line(buf, used);
			return 0;
		}
		used += n;

		while ((nl = memchr(start, '\n', buf + used - start))) {
			if (!discarding && handle_line(start, nl - start))
				return 0;
			discarding = 0;
			start = nl + 1;
		}
		used -= start - buf;
		memmove(buf, start, used);

		/* No newline in a full buffer: drop the rest of this line */
		if (used == sizeof(buf)) {
			LOG("message too long (max %d bytes)", MAX_PAYLOAD);
			discarding = 1;
			used = 0;
		}
	}
}

int main(int argc, char **argv)
{
	struct metal_init_params metal_param = METAL_INIT_DEFAULTS;
	struct resource_table *rsc;
	struct virtio_device *vdev;
	int irq = -1, ret = 1;
	long deadline;

	/* Keep received messages visible when stdout isn't a terminal */
	setvbuf(stdout, NULL, _IOLBF, 0);

	LOG("initializing OpenAMP over UIO");
	if (metal_init(&metal_param)) {
		LOG("metal_init failed");
		return 1;
	}

	if (open_uio(RSC_DEV_NAME, &plat.rsc_dev, &plat.rsc_io) ||
	    open_uio(SHM_DEV_NAME, &plat.shm_dev, &plat.shm_io) ||
	    open_uio(IPI_DEV_NAME, &plat.ipi_dev, &plat.ipi_io) ||
	    open_uio(BULK_DEV_NAME, &plat.bulk_dev, &plat.bulk_io))
		goto out;

	irq = (intptr_t)plat.ipi_dev->irq_info;
	metal_irq_register(irq, ipi_irq_handler, &plat);
	metal_irq_enable(irq);
	metal_io_write32(plat.ipi_io, IPI_IER_OFFSET, IPI_RPU0_MASK);

	if (wait_for_rsc_table()) {
		LOG("no resource table from the R5F at 0x%lx",
		    (unsigned long)metal_io_phys(plat.rsc_io, 0));
		goto out;
	}

	rsc = metal_io_virt(plat.rsc_io, 0);
	if (!remoteproc_init(&rproc, &rproc_ops, &plat)) {
		LOG("remoteproc_init failed");
		goto out;
	}
	add_mem(&plat.rsc_mem, "rsc_table", plat.rsc_io);
	add_mem(&plat.shm_mem, "shm", plat.shm_io);

	if (remoteproc_set_rsc_table(&rproc, rsc, metal_io_region_size(plat.rsc_io))) {
		LOG("failed to parse the resource table");
		goto out;
	}

	vdev = remoteproc_create_virtio(&rproc, 0, VIRTIO_DEV_DRIVER, NULL);
	if (!vdev) {
		LOG("failed to create the virtio device");
		goto out;
	}

	rpmsg_virtio_init_shm_pool(&shpool, metal_io_virt(plat.shm_io, SHM_BUF_OFFSET),
				   metal_io_region_size(plat.shm_io) - SHM_BUF_OFFSET);
	if (rpmsg_init_vdev(&rvdev, vdev, ns_bind_cb, plat.shm_io, &shpool)) {
		LOG("rpmsg_init_vdev failed");
		goto out;
	}
	LOG("vrings ready, waiting for the R5F to announce \"%s\" and \"%s\"",
	    RPMSG_SERVICE_NAME, RPMSG_BULK_SERVICE_NAME);

	deadline = now_ms() + NS_TIMEOUT_S * 1000L;
	while (!(ept_bound && bulk_ept_bound) && now_ms() < deadline)
		wait_for_events(-1);
	if (!(ept_bound && bulk_ept_bound)) {
		LOG("timed out waiting for the name service announcements");
		goto out_ept;
	}

	/* We bound straight to the R5F's addresses; tell it ours */
	if (rpmsg_send(&ept, "", 0) < 0 || rpmsg_send(&bulk_ept, "", 0) < 0) {
		LOG("failed to send the connect messages");
		goto out_ept;
	}
	LOG("connected to \"%s\" and \"%s\"", RPMSG_SERVICE_NAME, RPMSG_BULK_SERVICE_NAME);

	if ((argc > 1 ? run_cli(argc, argv) : run_interactive()))
		goto out_ept;

	LOG("done");
	ret = 0;

out_ept:
	if (bulk_ept_bound)
		rpmsg_destroy_ept(&bulk_ept);
	if (ept_bound)
		rpmsg_destroy_ept(&ept);
	rpmsg_deinit_vdev(&rvdev);
out:
	if (irq >= 0) {
		metal_io_write32(plat.ipi_io, IPI_IDR_OFFSET, IPI_RPU0_MASK);
		metal_irq_disable(irq);
		metal_irq_unregister(irq);
	}
	if (plat.bulk_dev)
		metal_device_close(plat.bulk_dev);
	if (plat.ipi_dev)
		metal_device_close(plat.ipi_dev);
	if (plat.shm_dev)
		metal_device_close(plat.shm_dev);
	if (plat.rsc_dev)
		metal_device_close(plat.rsc_dev);
	metal_finish();
	return ret;
}
