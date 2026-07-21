# EL2 HV Shell — `vm_console` attach + extensible debug commands Design

> **Status:** Draft — approved in the 2026-07-21 brainstorming session; not yet
> implemented.
> **Milestone:** Follow-up to M5 (multi-VM foundation). Builds on
> `hypervisor/dm/vuart.c`'s `console_focus` and `irq_handler.c`'s PL011
> RX-drain loop (see [[../adr/0014-multi-vm-static-partition-el2-console]]).

## Goal

Replace M5's "Ctrl-T cycles console focus" with an ACRN-style **EL2 shell**:
Ctrl-T becomes a two-state toggle that enters/exits a small command-line
interpreter running on the physical console. Inside the shell, `vm_console
<n>` explicitly sets which VM's vuart receives keyboard input once the shell
is exited; `help` lists commands. This is the seam M6+ debugging commands
(vm_list, vcpu_list, register dumps, ...) attach to, without changing this
milestone's actual command set.

**Observable DoD:** on the M5 dual-VM `run-qemu.sh` boot, pressing Ctrl-T from
either VM's shell prints `hv> ` and takes over the physical console;
typing `help` lists `help` and `vm_console`; typing `vm_console 1` prints
`[hv] console: VM1` and stays in the `hv> ` prompt; pressing Ctrl-T again
exits the shell and delivers keystrokes to VM1's vuart, which is now
interactive. The reverse (attach back to VM0) works identically. Backspace
during shell input editing works; an unknown command prints an error and
does not crash or hang.

---

## Decisions (from the 2026-07-21 brainstorming session)

| # | Decision | Resolution |
|---|---|---|
| 1 | Interaction model | A real EL2 shell (ACRN-style), not a bare loop-and-switch — commands are the extension point |
| 2 | Ctrl-T semantics | Two-state toggle: enter shell / exit shell (**not** a VM-cycling key anymore) |
| 3 | Exit-shell target | Exiting always returns to whatever `console_focus` currently holds — `vm_console` is the only thing that changes it |
| 4 | Line editing | Minimal: backspace only, no history, no arrow keys |
| 5 | Command set (this round) | Exactly two: `help`, `vm_console <n>` — extensible table, no other commands implemented now |
| 6 | Boot default | `console_focus = 0` (VM0), `shell_active = false` — identical to pre-existing M5 boot behavior |
| 7 | Attaching to an off VM | Allowed without validation; consistent with M5's existing known gap (focus on a shut-down VM is a silent no-op) |
| 8 | `vm_console` side effect | Only sets `console_focus`; does **not** auto-exit the shell — exiting is always via Ctrl-T |

---

## Architecture

```mermaid
sequenceDiagram
    participant U as Physical keystroke
    participant IH as irq_handler.c<br/>PL011 RX dispatch
    participant SH as hv_shell.c
    participant VU as vuart.c (vm[console_focus])

    U->>IH: byte from physical FIFO
    alt c == 0x14 (Ctrl-T)
        IH->>SH: shell_active ? hv_shell_exit() : hv_shell_enter()
    else shell_active == true
        IH->>SH: hv_shell_rx(c)
        Note over SH: line edit; on Enter, parse and dispatch<br/>vm_console <n> only sets console_focus
    else shell_active == false
        IH->>VU: vuart_rx(&vm[console_focus], c)
    end
```

Ctrl-T interception itself **stays in `irq_handler.c`'s RX-drain loop** — it
is the one place already responsible for "intercept a byte, never deliver it
to anyone," and both the shell and vuart are downstream of that decision, not
upstream of it. `hv_shell.c` becomes a new peer module to `vuart.c` under
`hypervisor/dm/`, mirroring ACRN's `debug/shell.c` role.

### `hypervisor/dm/hv_shell.h` (the only symbols other files need)

```c
extern bool shell_active;   /* irq_handler.c reads this to pick a dispatch */
void hv_shell_enter(void);  /* Ctrl-T while not in the shell: set + print "hv> " */
void hv_shell_exit(void);   /* Ctrl-T while in the shell: clear, no extra print */
void hv_shell_rx(u8 ch);    /* feed one byte: line-edit, or dispatch on Enter */
```

### Command table (`hv_shell.c`, private)

```c
struct hv_shell_cmd {
    const char *name;
    const char *help;
    void (*fn)(const char *arg);   /* arg = raw text after the command name */
};
static const struct hv_shell_cmd hv_shell_cmds[] = {
    { "help",       "list commands",                              cmd_help },
    { "vm_console", "vm_console <n> - attach console input to VM n", cmd_vm_console },
};
```

A future debug command is one function + one table row — no other file
changes. This is the explicit extension point the user asked for.

### Line editing

A fixed 128-byte line buffer + length counter, module-static to `hv_shell.c`:

- Printable ASCII (0x20–0x7E): append to buffer, echo the character.
- Backspace (`0x08` or `0x7F`, accept both): if buffer non-empty, drop the
  last character, echo `\b \b` (matches the terminal convention already
  implicit in this repo's uart handling — no cursor-position tracking, no
  mid-line insert, matching Decision 4's "minimal" scope).
- `\r` or `\n`: null-terminate, dispatch (see below), clear the buffer, print
  a fresh `hv> ` prompt.
- Buffer full (128 chars typed without Enter): same handling as `\r` — flush
  and reprompt, mirroring how `irq_handler.c`'s existing overflow-adjacent
  code paths fail safe rather than silently truncate forever.

### Command dispatch

On Enter: split the line at the first space into `name` + `arg` (arg may be
empty). Linear-search `hv_shell_cmds[]` by `name` (table has 2 entries — no
need for anything beyond `strcmp` in a loop, matching this codebase's
existing "small fixed tables, linear scan" style, e.g. the MMIO bus
registration in `mmio.c`). No match: print `Error: Invalid command.` (ACRN's
exact wording, kept for familiarity) and reprompt. Match: call `fn(arg)`.

`cmd_help`: prints each table entry's `name` + `help` on one line.

`cmd_vm_console`: parses `arg` as a decimal integer (a small
`u32 hv_shell_parse_u32(const char *)` helper — this repo has no `strtol`;
write the minimal decimal-only parser, reject non-digit input). If the
result is `>= NR_VMS` or `arg` is empty/non-numeric: print
`Error: invalid VM id.` and do not touch `console_focus`. Otherwise:
`console_focus = n;` and `printk("[hv] console: VM%u\n", n);` — the exact
message format already used by the (now-replaced) Ctrl-T-cycles-focus code,
so existing manual-test muscle memory and any external expectations about
that log line stay valid.

---

## Changes to existing files

- **`hypervisor/arch/arm64/irq/irq_handler.c`**: the PL011 RX-drain loop's
  Ctrl-T branch changes from cycling `console_focus` to calling
  `shell_active ? hv_shell_exit() : hv_shell_enter()`. The non-Ctrl-T byte
  path becomes `if (shell_active) hv_shell_rx((u8)c); else vuart_rx(&vm[console_focus], (u8)c);`.
  `#include "../../dm/hv_shell.h"` (or the project's existing relative-include
  convention for cross-directory `dm/` headers — match whatever `vuart.h`'s
  include already established there).
- **`hypervisor/dm/vuart.c` / `vuart.h`**: `console_focus`'s doc comment
  updates to reflect its narrowed role ("which VM receives input once the
  shell is exited," not "which VM currently receives input" — the shell can
  be active while `console_focus` sits unread). No behavioral change to
  `vuart_rx`/`vuart_write`/TX path.
- **`hypervisor/Makefile`**: add `dm/hv_shell.o` to `hv-objs` (mirrors how
  `dm/vuart.o` was added in M5 slice 2).
- **No Stage-2, vGIC, PSCI, or `vm_config.h` changes** — this is purely a
  console-input-routing feature on top of the existing M5 mechanism; it does
  not touch any VM lifecycle, memory, or interrupt path.

---

## Testing

No new automated QEMU scenario (matches this repo's existing gap for
Ctrl-T-adjacent behavior, noted in ADR-0014's Consequences — the shell adds
input-routing logic in the same category, not a new gap). Verification is
manual, on the M5 dual-VM boot:

1. Both VMs boot; VM0 has input focus (unchanged default).
2. Ctrl-T → `hv> ` appears; typing on either VM's shell no longer echoes
   there.
3. `help` → both commands listed with their help text.
4. `vm_console 1` → `[hv] console: VM1` printed, still at `hv> `.
5. `vm_console 9` (out of range for `NR_VMS=2`) → `Error: invalid VM id.`,
   `console_focus` unchanged (verify by re-running `vm_console` with no
   further changes and confirming the next Ctrl-T-exit still lands on VM1
   from step 4, not VM0).
6. Backspace during a partially-typed command works (type `vm_conso`,
   backspace 3 times, type `sole 0`, Enter → behaves as `vm_console 0`).
7. Ctrl-T → shell exits, VM1 (per step 4) receives keystrokes; confirm with
   an interactive command in VM1's shell.
8. Ctrl-T again → back in `hv> `; `vm_console 0` → Ctrl-T exit → VM0
   interactive again.
9. `make test` stays green (no existing scenario touches Ctrl-T or the
   shell, so this is a regression-safety check, not new coverage).

---

## Explicitly out of scope (this round)

- No command history, no arrow-key/cursor editing, no tab completion.
- No additional commands beyond `help`/`vm_console` (vm_list, vcpu_list,
  register dumps, memory dumps — all deferred, table is ready for them).
- No auto-detection of / warning about an `off` VM in `vm_console`'s target.
- No change to TX behavior, Stage-2, vGIC, or PSCI paths.
