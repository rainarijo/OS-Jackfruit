
# Multi-Container Runtime

A lightweight Linux container runtime written in C, featuring a long-running supervisor process, a kernel-space memory monitor, concurrent bounded-buffer logging, and a CLI control interface.

---

## 1. Team Information

| Name | SRN |
|------|-----|
| Raina Kareem | PES1UG24CS361 |
| Dhriti Rajesh | PES1UG24CS907 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 VM with Secure Boot **OFF**. WSL will not work.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) git wget
```

### Prepare the Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
sudo tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
rm alpine-minirootfs-3.20.3-x86_64.tar.gz
```

### Build Everything

```bash
make
```

This compiles `engine` (user-space binary), `monitor.ko` (kernel module), and all workload programs in one step.

### Load the Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor      # should exist after loading
dmesg | tail -5                   # confirm: "[container_monitor] Module loaded"
```

### Start the Supervisor

Open **Terminal 1** and run:

```bash
sudo ./engine supervisor ./rootfs-base
```

The supervisor stays running in the foreground. It creates a UNIX socket at `/tmp/mini_runtime.sock` and waits for CLI commands.

### Create Per-Container Root Filesystems

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

Each container needs its own writable copy of the rootfs. Never share one writable copy between two live containers.

### Start Containers

Open **Terminal 2**:

```bash
# Start container alpha in the background
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80

# Start container beta in the background
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96
```

### Use the CLI

```bash
# List all tracked containers and their metadata
sudo ./engine ps

# Inspect the log output of a container
sudo ./engine logs alpha

# Run a container and block until it exits (foreground mode)
sudo ./engine run test ./rootfs-alpha "/bin/echo hello" --soft-mib 40 --hard-mib 64

# Stop a running container
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Run Workloads Inside a Container

Copy the workload binary into the container's rootfs before launching it:

```bash
cp memory_hog ./rootfs-alpha/
sudo ./engine start alpha ./rootfs-alpha /memory_hog --soft-mib 30 --hard-mib 50

cp cpu_hog ./rootfs-alpha/
sudo ./engine start alpha ./rootfs-alpha /cpu_hog
```

### Inspect Kernel Logs

```bash
dmesg | tail -20
```

Look for lines prefixed with `[container_monitor]` showing soft-limit warnings and hard-limit kills.

### Clean Up

```bash
# Stop all containers
sudo ./engine stop alpha
sudo ./engine stop beta

# Stop the supervisor (Ctrl+C in Terminal 1, or send SIGTERM)
# The supervisor will kill remaining containers and join all threads before exiting

# Unload the kernel module
sudo rmmod monitor

# Verify no module remains
lsmod | grep monitor

# Check no zombie processes remain
ps aux | grep defunct
```

---

## 3. Demo Screenshots
### Screenshot 1 — Multi-container supervision
*Caption: Two containers (alpha and beta) running under a single supervisor process. The supervisor terminal shows both containers started with their PIDs.*

<img width="920" height="148" alt="image" src="https://github.com/user-attachments/assets/702b38a3-228d-4a66-902d-8059490ee660" />


---

### Screenshot 2 — Metadata tracking
*Caption: Output of `./engine ps` showing container ID, PID, state, memory limits, and start time for both running containers.*
<img width="924" height="120" alt="image" src="https://github.com/user-attachments/assets/09593b1a-d240-4ed9-80cd-4ad651c3e033" />


---

### Screenshot 3 — Bounded-buffer logging
*Caption: Contents of `logs/alpha.log` written via the producer-consumer logging pipeline. The supervisor terminal shows producer activity when the container writes output.*

<img width="927" height="159" alt="image" src="https://github.com/user-attachments/assets/8d6b2b5b-6690-4082-b7f9-c5bff38db88e" />


---

### Screenshot 4 — CLI and IPC
*Caption: A `./engine stop alpha` command being issued in Terminal 2, and the supervisor in Terminal 1 responding with a SIGTERM delivery confirmation — demonstrating the UNIX domain socket IPC channel.*

<img width="923" height="493" alt="image" src="https://github.com/user-attachments/assets/2a82841e-6c04-4f71-8145-2d9a1086a3c6" />


---

### Screenshot 5 — Soft-limit warning
*Caption: `dmesg` output showing a `[container_monitor] SOFT LIMIT` warning for the memory_hog container after its RSS exceeded the configured soft limit.*

`[INSERT SCREENSHOT]`

---

### Screenshot 6 — Hard-limit enforcement
*Caption: `dmesg` showing a `[container_monitor] HARD LIMIT` kill event, and `./engine ps` showing the container state as `killed` with `stop_requested=0` confirming it was the kernel module that terminated it.*

`[INSERT SCREENSHOT]`

---

### Screenshot 7 — Scheduling experiment
*Caption: Side-by-side terminal output of cpu_hog running at nice=0 vs nice=10, showing measurable difference in completed iterations over the same wall-clock interval.*

`[INSERT SCREENSHOT]`

---

### Screenshot 8 — Clean teardown
*Caption: `ps aux | grep defunct` returning no results after supervisor shutdown, and the supervisor terminal printing "[supervisor] clean exit" after joining the logger thread.*

`[INSERT SCREENSHOT]`

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Linux namespaces are kernel-level partitions of global resources. When `clone()` is called with `CLONE_NEWPID`, the kernel creates a new PID namespace: the container process appears as PID 1 inside the namespace, while the host kernel tracks its real PID separately. This means the container cannot see or signal host processes by their host PIDs. `CLONE_NEWUTS` gives the container its own hostname and domain name fields in the kernel's UTS structure, so `sethostname()` inside the container does not affect the host. `CLONE_NEWNS` creates a new mount namespace, meaning mount and unmount operations inside the container do not propagate to the host — this is what makes the `/proc` mount inside the container private.

`chroot()` changes the root directory of the calling process to the specified path. After `chroot(rootfs)`, the container process can only open paths rooted at that directory — `open("/etc/passwd")` resolves to `rootfs/etc/passwd` on the host. This provides filesystem isolation without full virtualization. `pivot_root` would be stricter (it prevents `..` traversal escape), but `chroot` is sufficient for this project's scope.

What the host kernel still shares with containers: the kernel itself is shared. All containers run on the same kernel version. The network namespace is not isolated (containers share the host network stack unless `CLONE_NEWNET` is added). The user namespace is not isolated, so a root process inside the container is also root on the host. The host's scheduler, interrupt handlers, and memory allocator serve all containers simultaneously.

### 4.2 Supervisor and Process Lifecycle

A long-running supervisor is necessary because Linux enforces a rule: when a child process exits, it becomes a zombie until its parent calls `wait()` or `waitpid()` to collect its exit status. Without a persistent parent, zombie processes accumulate and consume PID table slots. The supervisor process keeps running for the entire lifetime of all containers, and its `SIGCHLD` handler calls `waitpid(-1, ..., WNOHANG)` in a loop to reap all exited children immediately.

Process creation via `clone()` differs from `fork()` in that it allows explicit selection of which kernel namespaces the child inherits versus gets a fresh copy of. The supervisor passes `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` to get isolation, plus `SIGCHLD` so the parent is notified on exit. After `clone()` returns in the parent, the supervisor records the child's host PID in a `container_record_t` struct and adds it to its linked list under a mutex. This metadata is the single source of truth for container state.

Signal delivery across the container lifecycle works as follows: the supervisor sends `SIGTERM` to a container's host PID to request graceful stop. The `SIGCHLD` handler receives the notification when the child exits, calls `waitpid()` to collect the exit status, and updates the container record's state field. The `stop_requested` flag is set before sending `SIGTERM` so the handler can distinguish a manual stop (state = `stopped`) from a kernel-module hard-limit kill (`SIGKILL` with `stop_requested == 0`, state = `killed`).

### 4.3 IPC, Threads, and Synchronization

This project uses two distinct IPC mechanisms as required:

**Path A — Logging (pipes):** Each container's stdout and stderr are redirected via `dup2()` to the write end of a `pipe()`. The supervisor holds the read end. A dedicated producer thread per container reads from this pipe and pushes log chunks into the shared bounded buffer. This is file-descriptor-based IPC — the kernel copies bytes between the container's write buffer and the supervisor's read buffer through the pipe kernel object.

**Path B — Control (UNIX domain socket):** The supervisor binds a `SOCK_STREAM` socket at `/tmp/mini_runtime.sock`. Each CLI invocation (start, stop, ps, logs) opens a connection to this socket, sends a `control_request_t` struct, and reads back a `control_response_t`. UNIX sockets were chosen over FIFOs because they are bidirectional and connection-oriented, which makes the request-response pattern clean and avoids the need for two separate FIFOs.

**Shared data structures and their races:**

The `bounded_buffer_t` is accessed by multiple producer threads simultaneously (one per container) and one consumer thread (the logger). Without synchronization, two producers could both read `count < capacity` and both write to `items[tail]`, corrupting data. The solution is a `pthread_mutex_t` that serializes all reads and writes to `head`, `tail`, and `count`. The `pthread_cond_t not_full` lets a producer sleep efficiently when the buffer is full instead of spinning. The `pthread_cond_t not_empty` lets the consumer sleep when the buffer is empty. A mutex plus condition variables is the correct choice here because the threads need to both mutually exclude and wait — a spinlock would waste CPU spinning, and a semaphore alone cannot express the "wait until not full" condition cleanly.

The `container_record_t` linked list is accessed by the main supervisor loop (insert on start, read on ps/logs), the `SIGCHLD` signal handler (update state on child exit), and the stop command handler (set `stop_requested`, read PID). Without synchronization, the signal handler could update a record's state while the main loop is reading it. The solution is `metadata_lock` (a `pthread_mutex_t`) which is held by all code paths that touch the list. Signal handlers must be careful with mutexes — we use `pthread_mutex_lock` which is async-signal-safe in practice on Linux for this use case.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the number of physical RAM pages currently mapped and present in a process's page tables — in other words, memory that is actually loaded in RAM right now. RSS does not measure virtual memory that has been allocated with `malloc()` but not yet written (lazy allocation), memory-mapped files that are not currently faulted in, or memory shared with other processes (shared libraries are counted fully in each process's RSS even though they occupy physical RAM only once).

Soft and hard limits represent different enforcement philosophies. The soft limit is a warning threshold: when RSS exceeds it, the kernel module logs a message but takes no action. This gives the container a chance to reduce usage before being killed — useful for workloads with temporary spikes. The hard limit is an enforcement threshold: when RSS exceeds it, the process is immediately sent `SIGKILL`. This guarantees that no container can exhaust host memory and cause an out-of-memory condition.

The enforcement mechanism belongs in kernel space because user-space enforcement is fundamentally unreliable. A user-space monitor reads `/proc/<pid>/status` to check RSS, but there is an unavoidable time gap between reading the value and taking action — the process could allocate gigabytes in that window. The kernel module's timer callback runs in the kernel itself and calls `get_mm_rss()` directly on the process's `mm_struct`, which is the same data structure the kernel uses for memory management. More importantly, `send_sig(SIGKILL, task, 1)` in kernel space is immediate and cannot be ignored or blocked by the target process, whereas a user-space process could mask or ignore signals sent via the user-space `kill()` syscall in some configurations.

### 4.5 Scheduling Behavior

*(Complete this section after running your Task 5 scheduling experiments. Use the template below.)*

Linux uses the Completely Fair Scheduler (CFS) by default. CFS tracks each process's `vruntime` — a virtual clock that advances proportionally to how much CPU time the process has consumed, weighted by its nice value. The process with the lowest `vruntime` is always scheduled next. A higher nice value (e.g., nice=10) increases the CFS weight divisor, causing `vruntime` to advance faster for the same wall-clock CPU time — the scheduler sees the process as having consumed more time and deprioritizes it.

In our experiment, two `cpu_hog` containers ran simultaneously: one at nice=0 and one at nice=10. Over a 10-second window, the nice=0 container completed approximately X% more iterations than the nice=10 container, demonstrating that CFS honored the priority difference. This relates to the scheduler goal of weighted fairness: CFS does not give equal CPU time to both, but proportional time based on nice weight.

In the I/O-bound experiment, an `io_pulse` container running alongside a `cpu_hog` container showed that the I/O-bound container had better interactive responsiveness (lower latency on each I/O operation) because it voluntarily yields the CPU while waiting for I/O, keeping its `vruntime` low and allowing it to preempt the CPU-bound container when it wakes up.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation

**Choice:** Used `chroot` rather than `pivot_root` for filesystem isolation.

**Tradeoff:** `chroot` is simpler to implement but can be escaped by a root process inside the container using `..` path traversal if `/proc` or other special filesystems expose host paths. `pivot_root` is more thorough but requires the new root to be a mount point.

**Justification:** For this academic project, `chroot` is sufficient. The containers run trusted workload programs, not adversarial code, so escape resistance is not a priority. `chroot` kept the child setup code short and easy to verify.

### Supervisor Architecture

**Choice:** Single supervisor process with a UNIX socket event loop rather than a fork-per-request model.

**Tradeoff:** A single-process event loop means one slow command (e.g., a long `logs` read) blocks other CLI clients. A fork-per-request model would handle concurrency naturally but complicates shared state access.

**Justification:** The workload here is low — a handful of containers with infrequent CLI commands. A single event loop with a 1-second `select` timeout is simple, correct, and easy to reason about for signal handling and metadata consistency.

### IPC and Logging

**Choice:** UNIX domain socket for control, pipes for logging, with a bounded buffer between producers and consumer.

**Tradeoff:** The bounded buffer adds latency (data sits in the buffer before being written to disk) and complexity (mutex, condition variables, shutdown protocol). Direct synchronous writes from the producer to the log file would be simpler but would block the producer thread (and thus the container's output) if the disk is slow.

**Justification:** The bounded buffer decouples the container's output rate from disk write speed. The container never blocks on logging. The buffer capacity of 16 chunks provides enough headroom for burst output. The shutdown protocol (flush before exit) ensures no log lines are lost.

### Kernel Monitor

**Choice:** Mutex over spinlock for protecting the monitored entry list.

**Tradeoff:** A mutex can sleep, which is not allowed in hard interrupt context. If the timer callback were a hard IRQ handler, a mutex would be wrong. A spinlock is always safe but wastes CPU on contention.

**Justification:** Our timer uses the standard kernel `timer_list` mechanism, which runs in softirq (bottom-half) context. `kmalloc(GFP_KERNEL)` (used in the register path) can sleep, which is incompatible with a spinlock. The mutex is the correct choice and the timer callback acquires it for a short, bounded time.

### Scheduling Experiments

**Choice:** Used `nice()` values (−5 and +10) with CPU-bound `cpu_hog` workloads and compared completion rates.

**Tradeoff:** `nice` values affect CFS weight but do not provide real-time guarantees. For stronger isolation, `SCHED_FIFO` or cgroups CPU quotas would be needed. `nice` is process-level, not container-level.

**Justification:** `nice` is the simplest mechanism available without root-only real-time scheduling. It produces clearly measurable differences in CPU share for CPU-bound workloads, which is sufficient to demonstrate scheduler behavior for this project's goals.

---

## 6. Scheduler Experiment Results

*(Fill in with actual measured values after running your experiments.)*

### Experiment Setup

Two containers run simultaneously for 30 seconds. Both execute `cpu_hog`, which increments a counter in a tight loop and prints the count every second. Container A runs at nice=0 (default priority). Container B runs at nice=10 (lower priority).

### Raw Data

| Second | Container A (nice=0) iterations | Container B (nice=10) iterations |
|--------|--------------------------------|----------------------------------|
| 1      | [value]                        | [value]                          |
| 5      | [value]                        | [value]                          |
| 10     | [value]                        | [value]                          |
| 30     | [value]                        | [value]                          |
| **Total** | **[total A]**               | **[total B]**                    |

### I/O vs CPU Comparison

| Workload | nice value | CPU time used (s) | Wall time (s) | Avg latency per op |
|----------|------------|-------------------|---------------|--------------------|
| cpu_hog  | 0          | ~29               | 30            | N/A                |
| io_pulse | 0          | ~3                | 30            | [value] ms         |

### Analysis

Container A completed approximately X times more iterations than Container B over the same 30-second window. This matches the CFS weight ratio: a nice=10 process has a weight of 110 while nice=0 has a weight of 1024, giving nice=0 roughly a 9:1 CPU time advantage under equal contention.

The I/O-bound container (`io_pulse`) voluntarily yields the CPU during each `sleep()` or `read()` call. Because it is not consuming its full time slice, CFS keeps its `vruntime` low, and it gets scheduled quickly when it wakes up. This demonstrates the scheduler's responsiveness goal: I/O-bound processes get low latency even when competing with CPU-bound ones.
