# AymOS trace schema and transport

This document is the normative contract for trace schema 1 and framing version
1. The implementation is intentionally small: fixed records, a bounded static
ring, binary UART frames, and a strict host decoder. It is not a timing
measurement facility.

## Record layout

Every record is a naturally eight-byte-aligned, 32-byte little-endian value.
`kernel/include/aymos_trace.h` asserts its size, alignment, and every offset.

| Offset | Type | Field |
|---:|---|---|
| 0 | `u64` | monotonic guest tick |
| 8 | `u32` | attempted-event sequence |
| 12 | `u16` | schema version (`1`) |
| 14 | `u8` | event type |
| 15 | `u8` | flags (`0` in schema 1) |
| 16 | `u16` | task ID or `0xffff` |
| 18 | `u16` | related task ID or `0xffff` |
| 20 | `u32` | event value 0 |
| 24 | `u32` | event value 1 |
| 28 | `u32` | event value 2 |

Task zero is idle; user tasks are 1 through 4. A sequence number belongs to an
event attempt, not merely an accepted record. A drop therefore leaves a gap in
later stored records. Ticks may repeat but may not decrease.

## Events

All unused values are zero. `deadline` and other 64-bit values use value 0 as
the low word and value 1 as the high word.

| ID | Name | Task / related | Values |
|---:|---|---|---|
| 1 | `KERNEL_START` | invalid / invalid | 0, task-table size, 0 |
| 2 | `TASK_CREATE` | new / creator-or-invalid | kind, priority, initial state |
| 3 | `TASK_RELEASE` | task / invalid | absolute deadline low/high, job sequence |
| 4 | `TASK_SELECT` | selected / incumbent-or-invalid | READY mask, selection metadata, candidate count |
| 5 | `SELECT_CANDIDATE` | candidate / selected | deadline low/high, candidate metadata |
| 6 | `TASK_START` | task / invalid | deadline low/high, job sequence; first start of this task-slot occupant |
| 7 | `TASK_PREEMPT` | outgoing / incoming | source mask: 1 SysTick, 2 runtime create, 3 both |
| 8 | `TASK_YIELD` | task / invalid | zero |
| 9 | `TASK_SLEEP` | task / invalid | wake tick low/high, requested ticks |
| 10 | `TASK_WAKE` | task / invalid | deadline low/high, job sequence |
| 11 | `TASK_EXIT` | task / invalid | completed jobs, miss count, 0 |
| 12 | `CONTEXT_SWITCH` | outgoing-or-invalid / incoming | cause, outgoing state, 0 |
| 13 | `IDLE_START` | idle / outgoing-or-invalid | zero |
| 14 | `IDLE_STOP` | idle / incoming | zero |
| 15 | `DEADLINE_MET` | task / invalid | deadline low/high, job sequence |
| 16 | `DEADLINE_MISS` | task / invalid | deadline low/high, job sequence |
| 17 | `ALLOC` | owner / invalid | requested bytes, heap-relative pointer or `0xffffffff`, success boolean |
| 18 | `FREE` | owner / invalid | heap-relative pointer or sentinel, explicit-free subtype 1, success boolean |
| 19 | `TASK_WAIT_PERIOD` | task / invalid | next release low/high, 0 |
| 20 | `TASK_RELEASE_SKIPPED` | task / invalid | skipped cadence release low/high, cumulative skipped count |
| 21 | `OWNER_RELEASE` | owner / invalid | blocks released by PendSV, owner, success 1 |
| 22 | `TRACE_OVERFLOW` | invalid / invalid | dropped, attempted, emitted snapshot |

A deadline miss is emitted once when the scheduler first latches it, whether
that happens in SysTick or at completion. `DEADLINE_MET` is emitted only for a
completion whose job never latched a miss. Invalid allocator contexts rejected
before entering the allocator are not traced; accepted task calls emit success
or failure. PendSV owner cleanup has its separate event so it cannot be
mistaken for an explicit pointer free.

Context-switch causes are start 1, yield 2, sleep 3, exit 4, wait-period 5,
SysTick preemption 6, runtime-create preemption 7, and coalesced SysTick plus
runtime-create preemption 8. Task states use the public scheduler enum.

## Emission boundaries and ordering

The following boundaries are part of schema 1. "After" means that the named
kernel state has already been committed when the record is constructed;
"before" means that the following transition has not yet happened. Records at
one tick are ordered by sequence.

| Event | Execution context and state boundary |
|---|---|
| `KERNEL_START` | Thread mode after the kernel becomes running and before the start SVC. |
| `TASK_CREATE` | The task-create PRIMASK section, after the slot, schedule, initial frame, and output ID are published. |
| `TASK_RELEASE` | Task creation or SysTick, after the job becomes READY and its absolute deadline/job sequence are committed; before any resulting selection probe. |
| `TASK_SELECT` | The first record of an all-or-drop selection batch. A request probe records the state that requested PendSV; a dispatch record is emitted in PendSV after outgoing-state/reclaim work and selection, but before the selected task becomes RUNNING. |
| `SELECT_CANDIDATE` | Contiguous with its preceding `TASK_SELECT`, at the same tick and from the same snapshot. |
| `TASK_START` | PendSV after `CONTEXT_SWITCH`, after the selected user task is marked RUNNING, but before its first thread-mode instruction. It occurs once per task-slot occupant. |
| `TASK_PREEMPT` | PendSV only, after the actual outgoing and incoming tasks have been committed and after any `IDLE_STOP`/`IDLE_START`, immediately before `CONTEXT_SWITCH`. It describes the committed pair, never merely a request-site candidate. |
| `TASK_YIELD` | Yield SVC after RUNNING becomes READY and the yielding task becomes dispatch-excluded; before PendSV. |
| `TASK_SLEEP` | Sleep SVC after the task becomes SLEEPING and its wake tick is committed; before PendSV. |
| `TASK_WAKE` | SysTick after a sleeping task becomes READY and its job/deadline fields are committed; before any resulting selection probe. |
| `TASK_EXIT` | Exit SVC after deadline outcome/completion accounting and after RUNNING becomes EXITING; before PendSV reclaims the task. |
| `CONTEXT_SWITCH` | PendSV after selection and incoming RUNNING/current-task commit, after idle and committed-preemption records, and before first-start recording/register restore. |
| `IDLE_START` | PendSV after idle is committed RUNNING/current, before `TASK_PREEMPT` and `CONTEXT_SWITCH`. |
| `IDLE_STOP` | PendSV after a user task is committed RUNNING/current, before committed `TASK_PREEMPT` and `CONTEXT_SWITCH`. |
| `DEADLINE_MET` | Exit or wait-period SVC after completion accounting determines that the job never missed; immediately before `TASK_EXIT` or `TASK_WAIT_PERIOD`. |
| `DEADLINE_MISS` | SysTick when a live job first latches a miss, or exit/wait-period SVC when completion first latches it; one record per missed job and before the later lifecycle record. |
| `ALLOC` | The allocation API PRIMASK section after the allocator attempt, including failure, and before the section restores its caller's PRIMASK. Calls rejected before entering the allocator are not recorded. |
| `FREE` | The explicit-free API PRIMASK section after the allocator attempt, including failure, and before the section restores its caller's PRIMASK. |
| `TASK_WAIT_PERIOD` | Wait-period SVC after completion/deadline accounting and after the task enters its next-release wait; before PendSV. |
| `TASK_RELEASE_SKIPPED` | SysTick after a periodic task's occupied cadence release is accounted and the next cadence is advanced. |
| `OWNER_RELEASE` | PendSV after the bounded exiting-owner sweep succeeds and before the task slot is reset/reused; omitted when zero blocks were released. |
| `TRACE_OVERFLOW` | Immediately before a later single-event or selection-batch producer attempt, if prior loss is unreported and a slot is available. Its values snapshot the loss/counters before that later attempt. Reporting never calls itself recursively. |

Preemption requests accumulate as a deterministic bitwise OR until PendSV:
SysTick is bit 0 (value 1), runtime task creation is bit 1 (value 2), and both
is value 3. The matching context-switch causes are 6, 7, and 8. Request sites
emit selection probes but not `TASK_PREEMPT`; PendSV reselects from current
state and records the actual outgoing/incoming pair. This matters when a second
event changes the winner before PendSV runs.

Within PendSV the normative order is: dispatch selection batch; selected task
and `current_task` commit; `IDLE_STOP` or `IDLE_START` when applicable;
committed `TASK_PREEMPT` when applicable; `CONTEXT_SWITCH`; and first
`TASK_START` when applicable.

## Selection snapshots

A selection is one atomic ring batch: one `TASK_SELECT` immediately followed
by all of its `SELECT_CANDIDATE` records at the same tick. The batch is accepted
whole or dropped whole. The READY mask permits only user bits `0x1e`; idle,
the RUNNING incumbent, and the yield-excluded task are represented separately.

Selection metadata in summary value 1 is:

- bits 0-7: reason (`ONLY_READY=1`, `DEADLINE=2`, `PRIORITY=3`,
  `STABLE_TID=4`, `IDLE=5`, `EXCLUDED_FALLBACK=6`);
- bits 8-15: purpose (`DISPATCH=1`, `SYSTICK_PROBE=2`,
  `RUNTIME_CREATE_PROBE=3`);
- bits 16-23: excluded task, or `0xff`; and
- bits 24-31: eligible candidate count.

Candidate value 2 contains priority in bits 0-7, scheduler state in bits 8-15,
and boolean excluded, incumbent, and eligible markers in bits 16, 17, and 18.
All higher bits are reserved. User-incumbent probe snapshots include the
explicit RUNNING incumbent; dispatch snapshots contain READY tasks. An
idle-incumbent probe uses related task 0 but does not synthesize idle as a
candidate: all candidates are READY user tasks, no candidate carries the
incumbent marker, and the selected user must be present. An idle dispatch is
valid only with zero candidates, zero READY mask, no exclusion, `IDLE` reason,
and dispatch purpose. The reason is calculated
from the exact full-deadline, priority, stable-ID comparator and its runner-up.
The decoder replays that comparison only to validate the emitted explanation;
it does not invent absent candidate state.

## Ring, closure, and overflow

The firmware owns 256 static records (8192 bytes). A producer masks interrupts
while writing, preserves the incoming PRIMASK, performs no allocation, UART, or
formatting, and publishes a complete record/batch after a release fence. Every
producer loop is bounded by five task slots or five batch records. Consumer copies occur
under the same short critical section after an acquire fence; framing, CRC, and
polling UART transmission happen only after interrupts are restored.

When full, the ring preserves existing history and drops the whole new event or
selection batch in O(1) after bounded validation. Attempted, emitted, and
dropped counters saturate safely. Before every later producer attempt, the
runtime tries to report unreported loss. A full ring makes that report a
side-effect-free failure; once a slot is available exactly one nonrecursive
`TRACE_OVERFLOW` snapshot may be committed before the later attempt. That
record is supplementary: the footer is authoritative.

At the terminal idle boundary, one PRIMASK section changes the ring from open
to closed and snapshots counters, final sequence, flags, and final tick. Later
producers are rejected without changing counters. Idle thread mode then drains
already committed records and transmits the footer. No trace framing/flushing
UART runs in SVC, SysTick, PendSV, allocator critical sections, NMI, HardFault,
or panic paths. The pre-existing bounded panic diagnostic may still write its
ASCII panic line with interrupts disabled; it is not a trace producer or a
valid complete trace.

The trace app reserves 8192 bytes at `trace_storage` (`0x20001740` in the
current validated map), moving the linker heap start to `0x20003790`; the heap
still ends at `0x20017000`. This SRAM cost and all trace event work perturb the
guest. Trace output explains functional ordering in this Renode model; it does
not measure physical latency, WCET, clock accuracy, or hard-real-time bounds.

## UART framing

UART begins with the exact ASCII preamble:

```text
AYMOS READY\r\nTRACE BEGIN\r\n
```

Each binary frame is little-endian:

| Offset | Type | Meaning |
|---:|---|---|
| 0 | 4 bytes | sync `AYMT` |
| 4 | `u8` | framing version 1 |
| 5 | `u8` | type: record 1, footer 2 |
| 6 | `u16` | trace schema 1 |
| 8 | `u16` | payload length: 32 or 28 |
| 10 | `u16` | flags 0 |
| 12 | bytes | payload |
| after payload | `u32` | IEEE CRC32 over bytes 4 through payload end |

The 28-byte footer payload is attempted, emitted, dropped, final sequence, and
flags as five `u32` values followed by final guest tick as `u64`. A normal
complete run requires `attempted == emitted + dropped`, final sequence equal to
attempted minus one, flags zero, and dropped zero. The exact trailer follows:

```text
AYMOS TRACE DONE\r\n
```

Both the generic CLI and Renode workload validator use the same bounded file
reader. It accepts only a regular file, checks the open file size with `fstat`
before and after reading, and requests at most 1 MiB plus one byte. It rejects
oversized and sparse-over-limit inputs before parsing. It also rejects a size
change to the opened file during the read. A pathname replacement cannot alter
the descriptor that is already open. The reader does not claim to detect a
same-size content change. The decoder also caps parsing at 4096 frames. It
rejects unknown values,
invalid roles/payloads, CRC failure, sequence gaps, decreasing ticks,
truncation, duplicate/missing/nonterminal footer, loss, and trailing garbage.
Schema tests accept coherent records for event IDs 1 through 21. Event 22 is
validated but cannot appear in an accepted complete zero-loss stream: a
`TRACE_OVERFLOW` record or a nonzero footer drop count makes the run incomplete.
On corruption it scans at most 4096 bytes for the next sync solely to improve
the diagnostic; strict decoding never resumes or accepts a damaged stream.

The generic decoder does not impose one application's task schedule. Renode's
`verify_trace_uart.py` layers an exact all-record projection and state machine
for the finite `APP=trace` contract on the decoded records: 102 records, five
task lifecycles with task-slot reuse, a runtime-create/SysTick coalesced
preemption whose committed winner differs from the first request, three
idle-to-user preemptions, yield, two sleep/wake pairs, allocation/free, four
met deadlines, task 1's miss at tick/deadline 15, and the terminal switch to
idle. Mutation tests cover the required create/release, allocation/free,
preempt/context-switch, deadline/exit, and idle/context-switch ordered pairs.
Repeated-run byte/JSON equality is checked only after each attempt passes that
oracle.

## Commands and artifacts

```sh
make trace
make run-trace
make test-trace
RENODE_REPEAT=10 make test-trace
make APP=trace test-emulator-offline
make decode-trace TRACE_INPUT=uart.bin TRACE_JSON=trace.json \
  TRACE_BIN=trace.bin
```

`uart.bin` is the untouched mixed ASCII/framed UART capture. `trace.bin` is the
concatenation of decoded raw 32-byte record payloads; it excludes frame headers,
CRCs, footer, and ASCII. `trace.json` is the canonical decoded document and
retains each raw record as `raw_hex`. Emulator attempts also retain metadata,
commands, diagnostics, Robot output, and reference trace binary/JSON files.
Run/test metadata records SHA-256 hashes for both the generic decoder and the
workload validator, plus the schema/framing versions, record/footer sizes, and
ring capacity. `make decode-trace` invokes the locked project Python with user
site packages, `PYTHONPATH`, `PYTHONHOME`, and bytecode writes disabled.
Timeline construction and the PR 7 run-directory/report contract are outside
this PR.
