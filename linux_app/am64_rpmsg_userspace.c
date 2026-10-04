/*
 * A53 (Linux) side of the AM64x RPMsg POC: an OpenAMP RPMsg host (virtio
 * driver) in userspace. libmetal maps the resource table, the shared SRAM and
 * the IPI registers through UIO (see am64_rpmsg_overlay.dts).
 *
 * Flow: wait for the R5F to publish its resource table, initialize the vrings
 * and buffers (sets DRIVER_OK), wait for the R5F's name service announcement,
 * send "ping" and wait for the reply.
 */
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

#define BUS_NAME     "platform"
#define RSC_DEV_NAME "a0100000.rsc_table"
#define SHM_DEV_NAME "a5000000.shm"
#define IPI_DEV_NAME "ff340000.ipi"

/* ZynqMP IPI registers of the APU's channel 7 */
#define IPI_TRIG_OFFSET 0x00
#define IPI_ISR_OFFSET  0x10
#define IPI_IER_OFFSET  0x18
#define IPI_IDR_OFFSET  0x1c
#define IPI_RPU0_MASK   0x100 /* RPU0 is IPI channel 1 (bit 8) */

/* RPMsg buffers follow VRING0 (+0x0) and VRING1 (+0x4000) in the shared SRAM */
#define SHM_BUF_OFFSET 0x8000

#define RPMSG_SERVICE_NAME "rpmsg-client-sample"
#define MSG                "ping"

#define RSC_TABLE_TIMEOUT_S 30
#define REPLY_TIMEOUT_S     60

#define LOG(fmt, ...) printf("am64_rpmsg_userspace: " fmt "\n", ##__VA_ARGS__)

struct platform {
	struct metal_device *rsc_dev, *shm_dev, *ipi_dev;
	struct metal_io_region *rsc_io, *shm_io, *ipi_io;
	struct remoteproc_mem rsc_mem, shm_mem;
	atomic_int kicked;
};

static struct platform plat;
static struct remoteproc rproc;
static struct rpmsg_virtio_device rvdev;
static struct rpmsg_virtio_shm_pool shpool;
static struct rpmsg_endpoint ept;
static int ept_bound;
static int reply_received;

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

static int endpoint_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
		       uint32_t src, void *priv)
{
	(void)ept;
	(void)src;
	(void)priv;
	LOG("received \"%.*s\"", (int)len, (char *)data);
	reply_received = 1;
	return RPMSG_SUCCESS;
}

static void ns_bind_cb(struct rpmsg_device *rdev, const char *name, uint32_t dest)
{
	LOG("name service announcement: \"%s\" at address 0x%x", name, dest);
	if (strcmp(name, RPMSG_SERVICE_NAME))
		return;

	if (rpmsg_create_ept(&ept, rdev, name, RPMSG_ADDR_ANY, dest, endpoint_cb, NULL)) {
		LOG("failed to create endpoint");
		return;
	}
	ept_bound = 1;
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

/* Wait for a kick from the R5F and let OpenAMP process the vrings */
static void poll_notifications(void)
{
	if (atomic_exchange(&plat.kicked, 0))
		remoteproc_get_notification(&rproc, RSC_NOTIFY_ID_ANY);
	else
		usleep(1000);
}

static time_t now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

int main(void)
{
	struct metal_init_params metal_param = METAL_INIT_DEFAULTS;
	struct resource_table *rsc;
	struct virtio_device *vdev;
	int irq = -1, ret = 1;
	time_t deadline;

	LOG("initializing OpenAMP over UIO");
	if (metal_init(&metal_param)) {
		LOG("metal_init failed");
		return 1;
	}

	if (open_uio(RSC_DEV_NAME, &plat.rsc_dev, &plat.rsc_io) ||
	    open_uio(SHM_DEV_NAME, &plat.shm_dev, &plat.shm_io) ||
	    open_uio(IPI_DEV_NAME, &plat.ipi_dev, &plat.ipi_io))
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
	LOG("vrings ready, waiting for the R5F to announce \"%s\"", RPMSG_SERVICE_NAME);

	deadline = now() + REPLY_TIMEOUT_S;
	while (!ept_bound && now() < deadline)
		poll_notifications();
	if (!ept_bound) {
		LOG("timed out waiting for the name service announcement");
		goto out_vdev;
	}

	if (rpmsg_send(&ept, MSG, strlen(MSG)) < 0) {
		LOG("rpmsg_send failed");
		goto out_ept;
	}
	LOG("sent \"%s\"", MSG);

	while (!reply_received && now() < deadline)
		poll_notifications();
	if (!reply_received) {
		LOG("timed out waiting for the reply");
		goto out_ept;
	}

	LOG("done");
	ret = 0;

out_ept:
	rpmsg_destroy_ept(&ept);
out_vdev:
	rpmsg_deinit_vdev(&rvdev);
out:
	if (irq >= 0) {
		metal_io_write32(plat.ipi_io, IPI_IDR_OFFSET, IPI_RPU0_MASK);
		metal_irq_disable(irq);
		metal_irq_unregister(irq);
	}
	if (plat.ipi_dev)
		metal_device_close(plat.ipi_dev);
	if (plat.shm_dev)
		metal_device_close(plat.shm_dev);
	if (plat.rsc_dev)
		metal_device_close(plat.rsc_dev);
	metal_finish();
	return ret;
}
