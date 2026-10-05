# Antigravity Agent Orchestration Spec: AM64x RPMsg Stack in Renode (Userspace Pivot)

## 1. Project Objective

Implement and verify an Asymmetric Multiprocessing (AMP) RPMsg stack using the OpenAMP framework. The target hardware is the TI AM6442 SoC (simulated via ZynqMP platform stand-in). The simulation must run entirely within **Renode**.

**Stack Requirements:**
* **Master Core:** Cortex-A53 running a minimal generic Linux OS.
* **Remote Core:** Cortex-R5F running Zephyr RTOS.
* **IPC Mechanism:** OpenAMP (vrings + shared SRAM) operating in **Linux Userspace** via `libmetal` and `libopen_amp`.
* **Hardware Trigger:** Mocked TI hardware mailbox bound to the generic Linux UIO driver.

## 2. Agent Workflow & Execution Phases

### Phase 1: Workspace & Toolchain Refactoring

**Agent Task:** Modify the existing scaffolding to replace the A53 kernel module with a userspace application.

1. **Zephyr RTOS (R5F):**
   * Maintain the existing Zephyr workspace (`zephyr_app/`).
   * Ensure `main.c` listens for the RPMsg payload and `rsc_table.c` maps the memory accurately.

2. **Linux (A53) Userspace App:**
   * Delete `am64_rpmsg_client.c`.
   * Create `am64_rpmsg_userspace.c`. This must be a standard C application that uses `libmetal` and `libopen_amp` to initialize the vrings from shared memory and send a `"ping"` message to the remote core.
   * Update `am64_rpmsg_overlay.dts`. Instead of a custom kernel driver, define the shared SRAM and the mocked mailbox registers as nodes compatible with `"generic-uio"` (using the `uio_pdrv_genirq` driver).

3. **Docker Environment Update:**
   * Modify the `Dockerfile` to download/cross-compile `libmetal` and `libopen_amp` for the AArch64 target so the userspace application can link against them during the build phase.

### Phase 2: Memory Mapping (Unchanged)

**Agent Task:** Ensure both cores agree on the physical addresses.
* **Shared SRAM Base Address:** `0xA5000000`
* **Size:** `0x100000` (1MB)
* **Resource Table Offset:** `0xA0100000`
* **VRING0 & VRING1:** 4K aligned.

### Phase 3: Renode Simulation Configuration

**Agent Task:** Update the simulation scripts to accommodate the userspace execution.

1. **`am64_zynqmp.repl` & `ti_mailbox_mock.py`:**
   * Keep the existing `.repl` memory map and Python mailbox mock. The mock still triggers the R5F interrupt upon a TX register write.

2. **`run_poc.resc` (Startup Script):**
   * Load a pre-compiled generic ARM64 Linux kernel (`Image`) and a minimal rootfs (e.g., ext2/cpio).
   * Ensure the bootargs string includes `uio_pdrv_genirq.of_id="generic-uio"` to force the UIO driver to bind to the device tree overlay nodes.
   * Ensure the script loads the custom DTB and the Zephyr ELF into memory.

### Phase 4: Autonomous Build & Verification

**Agent Task:** Execute the build and simulation in the Docker container.

1. **Build:** 
   * Compile the Zephyr RTOS binary.
   * Cross-compile `am64_rpmsg_userspace.c` (linking `-lmetal` and `-lopen_amp`).

2. **Simulate:** 
   * Launch Renode headless via `build_and_run.sh`.
   * Inside the simulated Linux environment, run the compiled userspace binary (e.g., `./am64_rpmsg_userspace`).

3. **Validate (Acceptance Criteria):**
   * Monitor the UART outputs.
   * *Success:* The Linux console shows the userspace app sending the ping, and the Zephyr console prints `OpenAMP: Received message: "ping"`.
   * *Custom messages:* the Phase 5 acceptance cases also pass.

### Phase 5: Custom Messages (Both Directions)

**Agent Task:** Extend the fixed `"ping"`/`"pong"` exchange so that a user can send arbitrary text messages from Linux to Zephyr and from Zephyr to Linux over the existing `rpmsg-client-sample` endpoint. The transport (resource table, vrings, shared SRAM, IPI, memory map) stays the same.

1. **Message format:**
   * Payloads are plain text, without a NUL terminator on the wire. The RPMsg length field delimits them.
   * Maximum payload is 496 bytes (512-byte RPMsg buffer minus the 16-byte RPMsg header). Senders reject longer messages with an error and don't truncate them.
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
   * `XFER`: the sender has written `len` bytes at `offset` in its own area. It issues a write barrier before sending the descriptor.
   * The receiver validates the range against the sender's area, computes the IEEE CRC32 in place, logs the result, and answers `RELEASE` with the same id.
   * Only one transfer per direction is in flight. A new send is refused until `RELEASE` arrives. Malformed descriptors are logged and dropped.
4. **Linux app:**
   * `--bulk <file>` in CLI mode, processed in order with the text arguments. The app waits for `RELEASE` before continuing.
   * `/bulk <file>` in interactive mode.
   * Output lines: `sent bulk <id>: <len> bytes, crc 0x<crc>`, `received bulk <id>: <len> bytes, crc 0x<crc> OK|MISMATCH`, `bulk <id> released`.
5. **Zephyr:**
   * `rpmsg bulk <n>[K|M]` fills the R5F→A53 area with a test pattern and logs `OpenAMP: Sent bulk <id>: <len> bytes, crc 0x<crc>`.
   * Received payloads are logged as `OpenAMP: Received bulk <id>: <len> bytes, crc 0x<crc> OK|MISMATCH`.
6. **Acceptance test:** a third test case.
   * Create a 1 MB `/tmp/blob` with `dd`, send it with `/bulk`, and expect `OK` on Zephyr and `bulk 1 released` on Linux.
   * Run `rpmsg bulk 2M` and expect `OK` on Linux and `OpenAMP: bulk 1 released` on Zephyr.
   * Ping/pong, then `quit`.
7. **Real hardware:** the region must be non-cacheable for the R5F (MPU), or the code must do explicit cache maintenance. UIO already maps it uncached on Linux.

## 3. Constraints & Fallbacks
* **UIO Mapping:** The A53 userspace code must explicitly use `metal_device_open()` to map the UIO device exposed by the Linux kernel (typically `/dev/uio0`).
* **Rootfs Injection:** To avoid building a full Linux image, instruct Renode to inject the compiled `am64_rpmsg_userspace` binary directly into the virtual guest's root filesystem (using Renode's `sysbus LoadELF` or guest agent transfer mechanisms) before execution.
