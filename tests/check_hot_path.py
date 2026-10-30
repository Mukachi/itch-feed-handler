"""S4 of note 002, S5 of notes 003, 005, 007, 008 and 009, and S3 of note 004, checked in the compiled code.
Design: docs/design/002-order-book.md, docs/design/003-events-directory-status.md

Usage: check_hot_path.py <objdump> <hot_path_subject binary>

Applying an event, for every combination of D1 and D2 meant not to allocate
(their B and C), is checked with everything it reaches: every function called,
transitively, may call only memory copies, moves and comparisons and the stack
protector's abort, never through a pointer, and contains no syscall
instruction. As the gateway's check (its notes 002 and 003). Handling a decoded
message (note 003), the handler on the default book, is held to the same, and
so is decoding one (note 004), every alternative. Decoder B's one indirect
call is through its own table of decoding functions: allowed for B alone, its
targets checked as subjects themselves (they are the A functions' bodies).
A packet through the sequencer, and its timer (note 005), likewise. The
receive path (note 007): a pass of each arrangement, through to the handler,
may call besides only the socket's own system calls (recvmmsg, recvmsg for
D1's alternative, sendto for re-requests) and clock_gettime (the vDSO: no
system call with the TSC clocksource). The passes that decode do so with B
(the default), so B's indirect call, and its table's targets, as above.
Publishing (note 008): a message through the handler and its publisher, and a
change of the feed's state, held to the memory functions alone (memset
included: an update is cleared before it is filled); the publisher's tick may
also call kill, and read errno after it (its once-a-second check that each
consumer's process runs). Recording and monitoring (note 009): a feed pass with
the recorder attached (as the receive path, the socket's calls allowed, and,
the pass running the publisher's tick, its kill), the recorder's own call, and
the monitoring refresh: memory functions alone. Recovery (note 011, S6): the
program's pass on the handler sharing its publisher, as the recorded pass; a
slice of a snapshot, which may also call recv (reading the snapshot's socket,
errno after it), close and a futex wake (`syscall`: once, ending the spin, the
standby's rebuild thread woken) and clock_gettime (timing the recovery), and
decodes with B; and its monitoring refresh: memory alone.
"""
import re
import subprocess
import sys

SUBJECTS = ["itchfh_apply_ref_hash_levels_sorted", "itchfh_apply_ref_hash_levels_window",
            "itchfh_apply_ref_direct_levels_sorted", "itchfh_apply_ref_direct_levels_window",
            "itchfh_handle_default", "itchfh_decode_switch", "itchfh_decode_table", "itchfh_decode_simd",
            "itchfh_decode_simd_unpadded", "itchfh_sequence_packet", "itchfh_sequence_tick",
            "itchfh_feed_one_poll", "itchfh_feed_two_receive", "itchfh_feed_two_handle",
            "itchfh_publish_message", "itchfh_publish_feed", "itchfh_publish_tick",
            "itchfh_record_poll", "itchfh_record_datagram", "itchfh_monitor_refresh",
            "itchfh_program_poll", "itchfh_recovery_slice", "itchfh_recovery_refresh"]
INDIRECT_ALLOWED = {"itchfh_decode_table", "itchfh_feed_one_poll", "itchfh_feed_two_handle", "itchfh_record_poll",
                    "itchfh_program_poll", "itchfh_recovery_slice"}  # B, and the passes that decode with it
# __memcpy_chk, __memmove_chk: with a bounds check (_FORTIFY_SOURCE, Ubuntu's default).
MEMORY = {"memcpy@plt", "__memcpy_chk@plt", "memmove@plt", "__memmove_chk@plt", "memcmp@plt", "__stack_chk_fail@plt",
          "memset@plt", "__memset_chk@plt"}
SOCKET = {"recvmmsg@plt", "recvmsg@plt", "sendto@plt", "clock_gettime@plt"}
SNAPSHOT = {"recv@plt", "close@plt", "syscall@plt", "clock_gettime@plt", "__errno_location@plt"}  # a slice of a snapshot (note 011)
FEED = {"itchfh_record_poll", "itchfh_program_poll"}  # passes that also run the publisher's tick

failures = 0


def check(cond, what):
    global failures
    print(("pass" if cond else "FAIL") + "  " + what)
    if not cond:
        failures += 1


out = subprocess.run([sys.argv[1], "-d", "--no-show-raw-insn", sys.argv[2]],
                     check=True, capture_output=True, text=True).stdout
funcs, name = {}, None
for line in out.splitlines():
    m = re.match(r"^[0-9a-f]+ <(.*)>:$", line)
    if m:
        name = m[1]
        funcs[name] = []
        continue
    m = re.match(r"^\s+[0-9a-f]+:\s+(\S+)\s*(.*)$", line)
    if m and name is not None:
        funcs[name].append((m[1], m[2]))


def calls(ins):
    direct = {re.search(r"<([^>]*)>", o)[1].split("+")[0] for mnem, o in ins
              if (mnem.startswith("call") or mnem.startswith("jmp")) and "<" in o}
    indirect = [o for mnem, o in ins if mnem.startswith("call") and "<" not in o]
    return direct, indirect


for subject in SUBJECTS:
    if subject not in funcs:
        check(False, f"{subject} found")
        continue
    seen, todo, bad = set(), [subject], []
    if subject in INDIRECT_ALLOWED:  # its table's targets: every per-type decoding function
        targets = [f for f in funcs if "6detail6fields" in f]
        if len(targets) < 23:
            bad.append(f"{subject}: found {len(targets)} of its table's 23 decoding functions")
        todo += targets
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        direct, indirect = calls(funcs[f])
        if subject not in INDIRECT_ALLOWED:
            bad += [f"{f}: indirect call {o}" for o in indirect]
        bad += [f"{f}: {m}" for m, _ in funcs[f] if m in ("syscall", "sysenter", "int")]
        for t in direct:
            if t.endswith("@plt"):
                if t not in MEMORY and not ((subject.startswith("itchfh_feed") or subject in FEED) and t in SOCKET) and not (subject in ("itchfh_publish_tick",) + tuple(FEED) and t in ("kill@plt", "__errno_location@plt")) and not (subject == "itchfh_recovery_slice" and t in SNAPSHOT | SOCKET):
                    bad.append(f"{f}: calls {t}")
            elif t in funcs and t != f:
                todo.append(t)
    size = sum(len(funcs[f]) for f in seen)
    label = ("S5 (note 003)" if subject.startswith("itchfh_handle") else "S3 (note 004)" if subject.startswith("itchfh_decode")
             else "S5 (note 005)" if subject.startswith("itchfh_sequence") else "S5 (note 007)" if subject.startswith("itchfh_feed")
             else "S5 (note 008)" if subject.startswith("itchfh_publish")
             else "S5 (note 009)" if subject.startswith("itchfh_record") or subject.startswith("itchfh_monitor")
             else "S6 (note 011)" if subject.startswith("itchfh_program") or subject.startswith("itchfh_recovery")
             else "S4")
    check(not bad, f"{label} {subject}: {len(seen)} functions, {size} instructions, calls only memory functions "
                   f"and the stack protector" + (", and the socket's calls" if subject.startswith("itchfh_feed") or subject in FEED or subject == "itchfh_recovery_slice" else "") + (f": found {bad[:5]}" if bad else ""))

print("FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)
