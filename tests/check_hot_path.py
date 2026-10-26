"""S4 of note 002, S5 of note 003 and S3 of note 004, checked in the compiled code.
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
"""
import re
import subprocess
import sys

SUBJECTS = ["itchfh_apply_ref_hash_levels_sorted", "itchfh_apply_ref_hash_levels_window",
            "itchfh_apply_ref_direct_levels_sorted", "itchfh_apply_ref_direct_levels_window",
            "itchfh_handle_default", "itchfh_decode_switch", "itchfh_decode_table", "itchfh_decode_simd",
            "itchfh_decode_simd_unpadded"]
INDIRECT_ALLOWED = {"itchfh_decode_table"}
# __memcpy_chk, __memmove_chk: with a bounds check (_FORTIFY_SOURCE, Ubuntu's default).
MEMORY = {"memcpy@plt", "__memcpy_chk@plt", "memmove@plt", "__memmove_chk@plt", "memcmp@plt", "__stack_chk_fail@plt"}

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
                if t not in MEMORY:
                    bad.append(f"{f}: calls {t}")
            elif t in funcs and t != f:
                todo.append(t)
    size = sum(len(funcs[f]) for f in seen)
    label = "S5 (note 003)" if subject.startswith("itchfh_handle") else "S3 (note 004)" if subject.startswith("itchfh_decode") else "S4"
    check(not bad, f"{label} {subject}: {len(seen)} functions, {size} instructions, calls only memory functions "
                   f"and the stack protector" + (f": found {bad[:5]}" if bad else ""))

print("FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)
