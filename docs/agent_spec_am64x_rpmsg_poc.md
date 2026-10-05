# Antigravity Agent Orchestration Spec: AM64x RPMsg Stack in Renode (Userspace Pivot)

## 1. Project Objective

Implement and verify an Asymmetric Multiprocessing (AMP) RPMsg stack using the OpenAMP framework. The target hardware is the TI AM6442 SoC. Renode has no AM64x model, so a **Xilinx ZynqMP** platform stands in for it: it also has Cortex-A53 and Cortex-R5F cores. The simulation must run entirely within **Renode**.

**Stack Requirements:**
* **Host Core:** Cortex-A53 running Linux (Antmicro's prebuilt ZynqMP OpenAMP demo kernel and rootfs, Linux 6.6.10).
* **Remote Core:** Cortex-R5F running Zephyr RTOS.
* **IPC Mechanism:** OpenAMP (vrings + shared SRAM) operating in **Linux Userspace** via `libmetal` and `libopen_amp`. Payloads too large for RPMsg go through a separate shared-memory bulk region (Phase 6).
* **Hardware Trigger:** the stock ZynqMP **IPI** (inter-processor interrupt) block stands in for the TI hardware mailbox. Linux drives it from userspace through the generic UIO driver. Zephyr drives it through its `xlnx,mbox-versal-ipi-mailbox` MBOX driver.

## 2. Agent Workflow & Execution Phases

### Phase 1: Workspace & Toolchain Refactoring

**Agent Task:** Replace the A53 kernel module with a userspace application.

1. **Zephyr RTOS (R5F):**
   * The Zephyr app lives in `zephyr_app/` and is built for `qemu_cortex_r5`, which is the ZynqMP RPU board.
   * `main.c` uses raw OpenAMP in the remote (`VIRTIO_DEV_DEVICE`) role, not Zephyr's IPC service. `rsc_table.c` holds the resource table: one RPMsg vdev with fixed vring addresses and `VIRTIO_RPMSG_F_NS`.
   * Nothing loads the firmware through remoteproc, so the A53 can't read the resource table from the ELF. At boot, Zephyr copies the table to a fixed address that the A53 maps.

2. **Linux (A53) Userspace App:**
   * `am64_rpmsg_client.c` (the kernel driver) is deleted on purpose. Don't reintroduce it.
   * `am64_rpmsg_userspace.c` is a standard C application that uses `libmetal` and `libopen_amp` in the host (`VIRTIO_DEV_DRIVER`) role to initialize the vrings in shared memory and exchange messages with the remote core.
   * `am64_rpmsg_overlay.dts` declares the resource table, the shared SRAM, the bulk region and the APU's IPI channel as `"generic-uio"` nodes (bound by `uio_pdrv_genirq`). It also disables the nodes that would otherwise claim the same resources: the kernel's `zynqmp_ipi1` mailbox, the `rf5ss` remoteproc, and `uart0`, which is the Zephyr console.

3. **Docker Environment:**
   * The `Dockerfile` is based on `zephyrprojectrtos/ci`, which already ships Renode 1.16, the Zephyr SDK, `dtc` and `debugfs`.
   * It adds the AArch64 glibc cross compiler and builds `libsysfs`, `libmetal` and `open-amp` (`v2024.05.0`) as static libraries into the `/usr/aarch64-linux-gnu` sysroot, so the app can be linked statically.
   * The Zephyr side uses the open-amp module that ships with Zephyr (`v2026.04`). Its wire format (resource table, vrings, RPMsg header) is compatible with `v2024.05.0`.

### Phase 2: Memory Mapping

**Agent Task:** Ensure both cores agree on the physical addresses and the IPI assignment. These values are spread across `renode/am64_zynqmp.repl`, `zephyr_app/app.overlay`, `zephyr_app/src/rsc_table.c`, `linux_app/am64_rpmsg_overlay.dts`, `linux_app/am64_rpmsg_userspace.c` and `common/rpmsg_bulk.h`, so they must change together.
* **R5F (Zephyr) RAM:** `0xA0000000`, 1 MB, outside the DDR that Linux owns.
* **Resource table:** `0xA0100000`, 4 KB.
* **Shared SRAM:** `0xA5000000`, size `0x100000` (1 MB).
  * **VRING0 & VRING1:** 4K aligned, 256 descriptors each, at `+0x0` and `+0x4000`.
  * **RPMsg buffers:** from `+0x8000`.
* **Bulk region (added in Phase 6):** `0xA8000000`, size `0x1000000` (16 MB). A53→R5F area at `+0x0`, R5F→A53 area at `+0x800000`, 8 MB each.
* **APU IPI channel 7** (Linux side, UIO): `0xFF340000`, bit 24, SPI 29.
* **RPU0 IPI channel 1** (Zephyr side): `0xFF310000`, bit 8 (`0x100`), SPI 33.

### Phase 3: Renode Simulation Configuration

**Agent Task:** Configure the platform and startup script for the userspace stack.

1. **`am64_zynqmp.repl`:**
   * Includes Renode's stock `platforms/cpus/zynqmp.repl` and adds four `MappedMemory` regions: the R5F RAM, the resource table, the shared SRAM and the bulk region.
   * The mailbox is the stock `sysbus.ipi` (`ZynqMP_IPI`). The earlier Python mailbox mock (`ti_mailbox_mock.py`) was removed on purpose, because a `PythonPeripheral` can't drive IRQ lines.

2. **`run_poc.resc` (Startup Script):**
   * Follows Antmicro's `scripts/single-node/zynqmp_openamp.resc`. It loads ATF, U-Boot and the Linux `Image` (downloaded from `dl.antmicro.com`) on `apu0`, loads the DTB built from our overlay, and loads `build/rootfs.ext2` as an initrd (`root=/dev/ram0`).
   * The bootargs include `uio_pdrv_genirq.of_id=generic-uio`. `uio_pdrv_genirq` is a module, so the rootfs init script also runs `modprobe uio_pdrv_genirq of_id=generic-uio`.
   * Loads the Zephyr ELF on `rpu0`. The Linux console is `uart1`, the Zephyr console and shell are `uart0`.
   * Installs a `SetHookBeforePeripheralWrite` hook on `ipi`. Renode's `ZynqMP_IPI` doesn't implement write-1-to-clear on its ISR registers, and Zephyr's driver writes `0xFFFFFFFF` to ISR at init. Without the hook, that causes an interrupt storm on the first kick. Keep the hook.

### Phase 4: Autonomous Build & Verification

**Agent Task:** Execute the build and simulation in the Docker container with `scripts/build_and_run.sh [test|interactive|build]`.

1. **Build:**
   * Create the west workspace in `zephyrproject/` (Zephyr `v4.5.0-rc1`, only the open-amp, libmetal and cmsis modules) and build the Zephyr ELF.
   * Cross-compile `am64_rpmsg_userspace.c` statically (`-lopen_amp -lmetal -lsysfs`).
   * Merge the DT overlay into Antmicro's ZynqMP OpenAMP DTB with `fdtoverlay`.
   * Copy Antmicro's OpenAMP rootfs to `build/rootfs.ext2` and inject the app (`/root/am64_rpmsg_userspace`) and the init script with `debugfs`.

2. **Simulate:**
   * `test` (default) runs the acceptance test headless with `renode-test renode/rpmsg_poc.robot`. Results go to `build/test-results/`.
   * `interactive` starts Renode with the simulation running. The Linux console is served on TCP port 3456 and the Zephyr shell on 3457. Log in as `root` and run `/root/am64_rpmsg_userspace`.

3. **Validate (Acceptance Criteria):**
   * Monitor the UART outputs.
   * *Success:* The Linux console shows the userspace app sending `"ping"`, the Zephyr console prints `OpenAMP: Received message: "ping"`, and the app receives `"pong"`.
   * *Custom messages:* the Phase 5 acceptance cases also pass.
   * *Bulk transfers:* the Phase 6 acceptance case also passes.

### Phase 5: Custom Messages (Both Directions)

**Agent Task:** Extend the fixed `"ping"`/`"pong"` exchange so that a user can send arbitrary text messages from Linux to Zephyr and from Zephyr to Linux over the existing `rpmsg-client-sample` endpoint. The transport (resource table, vrings, shared SRAM, IPI, memory map) stays the same.

1. **Message format:**
   * Payloads are plain text, without a NUL terminator on the wire. The RPMsg length field delimits them.
   * Maximum payload is 496 bytes (512-byte RPMsg buffer minus the 16-byte RPMsg header). Senders reject longer messages with an error and don't truncate them. Larger payloads use the bulk channel (Phase 6).
   * A **zero-length message** from the A53 is a *connect* notification, not user data (see "Endpoint addressing" below). Neither side prints it as a received message.
   * Receivers print payloads with `%.*s` using the received length, so they never rely on NUL termination.

2. **Endpoint addressing:**
   * The A53 binds its endpoint straight to the address Zephyr announced, so it never announces itself. Zephyr learns the A53's address only from the first message it receives (OpenAMP fills `ept->dest_addr` on receive).
   * As soon as the endpoint is bound, the A53 app therefore sends one zero-length connect message. Zephyr logs `OpenAMP: A53 connected (addr 0x<addr>)`.
   * Until then, Zephyr-originated sends fail with a clear "A53 not connected" error and are not queued.

3. **Linux (A53) userspace app, `am64_rpmsg_userspace`:**
   * **CLI mode** (`am64_rpmsg_userspace <msg> [<msg> ...]`): sends each argument as a separate message, in order. It then prints incoming messages until none has arrived for 2 s, and exits with status 0. Quote arguments that contain spaces.
   * **Interactive mode** (no arguments): reads stdin line by line and sends each non-empty line without its trailing newline. Incoming messages are printed as they arrive, interleaved with input. The app polls stdin and the IPI kick flag together, so neither blocks the other. It exits on `quit` or EOF (Ctrl-D).
   * Output lines (relied on by the acceptance test):
     * `am64_rpmsg_userspace: sent "<text>"`
     * `am64_rpmsg_userspace: received "<text>"`
     * `am64_rpmsg_userspace: done` on a clean exit
   * The existing connection timeouts (resource table, name-service announcement) are unchanged. The fixed reply timeout no longer applies.

4. **Zephyr (R5F) firmware:**
   * Enable the Zephyr shell on the console UART (uart0) and add an `rpmsg` command group:
     * `rpmsg send <text...>`: joins the remaining arguments with single spaces and sends them to the A53. Prints `OpenAMP: Sent message: "<text>"` on success, or an error (not connected, too long, send failure). Shell line length (`CONFIG_SHELL_CMD_BUFF_SIZE`) may cap messages typed here below 496 bytes. That's acceptable.
   * Every received non-empty message is logged as `OpenAMP: Received message: "<text>"`.
   * **`"ping"` is the only auto-reply:** an exact `"ping"` is answered with `"pong"` and logged as `OpenAMP: Sent reply: "pong"`. Other messages get no automatic reply.
   * Sends from the shell thread call `rpmsg_send()` directly. OpenAMP serializes vring access with the RPMsg device lock, and the main thread keeps processing IPI kicks. The IPI `-EBUSY` retry loop in `mailbox_notify` must stay safe to call from either thread.

5. **Limitations (carried over or new):**
   * Still one RPMsg session per simulation boot: the R5F doesn't handle a vdev reset. Either CLI mode or one interactive session can run, but not both, without restarting the simulation.
   * After the A53 app exits, Zephyr still holds the A53's address. Further `rpmsg send` calls fill vring buffers that nobody reads. They are silently lost, and once the buffers run out the send times out with an error.

6. **Acceptance test (`renode/rpmsg_poc.robot`):** each test case boots a fresh simulation, because of the one-session-per-boot limit.
   * *Ping/pong (original criterion, CLI mode):* run `am64_rpmsg_userspace ping`. Expect `sent "ping"` on Linux, `OpenAMP: Received message: "ping"` on Zephyr, `received "pong"` and `done` on Linux.
   * *Custom messages (interactive mode):* start the app with no arguments, then:
     * Linux → Zephyr: type a custom line such as `hello from linux`. Expect `sent "hello from linux"` on Linux, `OpenAMP: Received message: "hello from linux"` on Zephyr, and no automatic reply.
     * Zephyr → Linux: write `rpmsg send hello from zephyr` to uart0. Expect `OpenAMP: Sent message: "hello from zephyr"` on Zephyr and `received "hello from zephyr"` on Linux.
     * Ping still works mid-session: type `ping` and expect `received "pong"`.
     * Type `quit` and expect `done`.

### Phase 6: Bulk Transfers (MB-Range Payloads)

**Agent Task:** RPMsg payloads are capped at 496 bytes. Add a bulk channel for payloads of up to 8 MB that uses RPMsg only for control and keeps the payload in shared memory (zero-copy, the pattern TI recommends for AM64x). The text endpoint and its behavior stay unchanged.

1. **Memory:** a 16 MB region at `0xA8000000`, declared in the Renode platform, the Zephyr overlay and the Linux DT overlay (`generic-uio`, opened as `a8000000.bulk`). It is split by direction, and each side writes only its own area: A53→R5F at `+0x0` and R5F→A53 at `+0x800000`, 8 MB each.
2. **Control endpoint:** Zephyr creates a second endpoint, `rpmsg-bulk`. The A53 waits for both name-service announcements and sends a zero-length connect message on each.
3. **Protocol** (`common/rpmsg_bulk.h`): `struct rpmsg_bulk_msg { type, id, offset, len, crc32 }`, all `uint32_t`.
   * Both sides number their transfers independently, starting at 1 on every boot.
   * `XFER`: the sender has written `len` bytes at `offset` in its own area. It issues a write barrier before sending the descriptor.
   * The receiver validates the range against the sender's area, computes the IEEE CRC32 in place, logs the result, and answers `RELEASE` with the same id.
   * Only one transfer per direction is in flight. A new send is refused until `RELEASE` arrives. Malformed descriptors are logged and dropped.
4. **Linux app:**
   * `--bulk <file>` in CLI mode, processed in order with the text arguments. The app waits for `RELEASE` before continuing.
   * `/bulk <file>` in interactive mode. Every other line is still sent as a text message.
   * Errors, logged without sending: file can't be opened or read, empty file, file larger than 8 MB, or a previous transfer still in flight. In CLI mode, waiting for `RELEASE` times out after 60 s, and the app exits with an error.
   * The bulk region is mapped uncached (device memory), so the app only accesses it through `metal_io_block_read/write` and never with plain `memcpy`.
   * Each received payload is saved to `/tmp/bulk_<id>.bin`, so it can be viewed after the app exits.
   * Output lines: `sent bulk <id>: <len> bytes, crc 0x<crc>`, `received bulk <id>: <len> bytes, crc 0x<crc> OK|MISMATCH`, `saved bulk <id> to /tmp/bulk_<id>.bin`, `bulk <id> released`.
5. **Zephyr:**
   * `rpmsg bulk <n>[K|M]` fills the R5F→A53 area with a test pattern (byte `i` is `(i * 31 + id) & 0xff`) and logs `OpenAMP: Sent bulk <id>: <len> bytes, crc 0x<crc>`. Zephyr has no files, so it can't send content of the user's choice. It fails with an error if the size is outside 1 byte to 8 MB, if the A53 isn't connected, or if a previous transfer is still in flight.
   * Received payloads are logged as `OpenAMP: Received bulk <id>: <len> bytes, crc 0x<crc> OK|MISMATCH`.
   * `rpmsg dump [len] [offset]` hexdumps the last payload received from the A53 in place (default: the first 256 bytes). It prints `OpenAMP: bulk <id>, bytes <first>..<last> of <len>:` followed by hexdump lines. The data is valid until the A53's next bulk send overwrites its area.
6. **Acceptance test:** a third test case.
   * Create a 1 MB `/tmp/blob` with `dd`, send it with `/bulk`, and expect `OK` on Zephyr and `bulk 1 released` on Linux. Then `rpmsg dump 32` shows the header and two hexdump lines.
   * Run `rpmsg bulk 2M` and expect `OK` and `saved bulk 1 to /tmp/bulk_1.bin` on Linux, and `OpenAMP: bulk 1 released` on Zephyr.
   * Ping/pong, then `quit`. `head -c 16 /tmp/bulk_1.bin | hexdump -C` shows the start of Zephyr's pattern (`01 20 3f 5e …`).
7. **Limitations:**
   * The R5F has 1 MB of RAM, so it never copies a payload. It checks and dumps payloads in place in the bulk region.
   * The receiver doesn't print payload contents on arrival, only the size and CRC result. Contents are viewed with `rpmsg dump` on Zephyr, or with the saved file on Linux after the app exits.
   * The Phase 5 limits still apply: one session per boot, and a Zephyr `rpmsg bulk` after the A53 app exits isn't received. Its descriptor is lost, and the transfer stays in flight until the next boot.
8. **Real hardware:** the region must be non-cacheable for the R5F (MPU), or the code must do explicit cache maintenance. UIO already maps it uncached on Linux.

## 3. Constraints & Fallbacks
* **UIO Mapping:** The A53 userspace code opens each UIO device by name on the `platform` bus with `metal_device_open()` (`a0100000.rsc_table`, `a5000000.shm`, `a8000000.bulk`, `ff340000.ipi`) instead of hardcoding `/dev/uioN` numbers.
* **Rootfs Injection:** To avoid building a full Linux image, `build_and_run.sh` copies Antmicro's prebuilt OpenAMP rootfs and writes the static `am64_rpmsg_userspace` binary and the `uio_pdrv_genirq` init script into it with `debugfs` at build time. That rootfs already ships a `uio_pdrv_genirq.ko` matching the kernel.
* **External Images:** The base DTB, rootfs, kernel and firmware URLs are pinned. Antmicro has republished these files under new hashes before, so if a download returns 404, recheck the URLs in `build_and_run.sh` and `run_poc.resc`.
* **One Session per Boot:** The R5F doesn't handle a vdev reset. Each acceptance test case boots a fresh simulation (`Test Setup  Reset Emulation`).
