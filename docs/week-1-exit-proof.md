# Week-1 exit proof: live-coding loop end-to-end

This is the artifact week 1 of the wide-beta plan calls for:

> *"One transcript that defines a small function, uses a conditional or
> loop, saves it, restores it, and sees it again."*

The session below was recorded against `build/host/frothy-host-normal`
(the host_normal profile — 32-bit tagged words, full feature set
including cells, text, persistence). The binary is a stdio REPL; input
lines were fed in via shell redirect. Reproduce with:

```sh
make host-normal
build/host/frothy-host-normal < docs/week-1-exit-proof.input
```

The input file is committed next to this document. The output below is
the literal capture.

## What the session proves

1. Source-level definitions land in slots (`counter`, `bump`, `greeting`,
   `fill`).
2. `set counter to counter + 1` mutates a top-level slot through a function
   body, using both the T4b mutation form and arithmetic — the inspected
   bytecode shows `LOAD_SLOT 29; PUSH_INT 1; ADD_INT; STORE_SLOT 29`.
3. A loop runs inside a function body (`repeat 3 [ ... ]` in `fill`).
4. `save` persists the slot image to storage.
5. `clear` wipes the runtime — user names are gone, calls return `err 7`
   (not found).
6. `restore` reads the image back. Every user-declared slot returns to its
   saved value, including the compiled function `bump`, which then runs
   again and mutates the restored counter from `3` to `4`. The compiled
   `fill` function still runs its loop after restore and produces the same
   `3`.

## The transcript

Each line that starts with `>` is the REPL prompt; what follows on the
same line is the response (a value, an error code, or empty); `ok`
on the next line marks command success.

```text
> frothy status v1 profile=host_normal profile_hash=7561b535 compiler=device names=device storage=eeprom interrupt=cooperative word_size=32 int_min=-1073741824 int_max=1073741823 apply_bytes=252
ok
> ok
> ok
> ok
> ok
> ok
> 3
ok
> ok
> ok
> ok
> 3
ok
> 3
ok
> overlay code
LOAD_SLOT 29
PUSH_INT 1
ADD_INT
STORE_SLOT 29
RETURN
ok
> ok
> ok
> err 7
> err 7
> err 7
> ok
> 3
ok
> "frothy beta lives"
ok
> ok
> 4
ok
> ok
> 3
ok
> 3
ok
```

## Annotated walk-through

Pairing each prompt with the source line that produced it:

| # | Source line | What the REPL did |
|---|---|---|
| 1  | `status` | reported profile, hash, storage, partition limits |
| 2  | `counter is 0` | declared an int slot at 0 |
| 3  | `bump is fn [ set counter to counter + 1 ]` | declared a function that reads counter, adds 1, stores it back |
| 4  | `bump:` | counter is now 1 (return value dropped at call) |
| 5  | `bump:` | counter is now 2 |
| 6  | `bump:` | counter is now 3 |
| 7  | `counter` | confirmed `3` |
| 8  | `greeting is "frothy beta lives"` | declared a text slot |
| 9  | `pad.reset` | cleared the pad buffer |
| 10 | `fill is fn [ repeat 3 [ pad.emit-byte: 65 ]; pad.len: ]` | declared a function that uses `repeat` to write `A` three times and returns the pad length |
| 11 | `fill:` | ran the loop; pad now has three bytes |
| 12 | `pad.len:` | confirmed `3` |
| 13 | `see bump` | inspected the compiled body, showing `LOAD_SLOT`, `PUSH_INT 1`, `ADD_INT`, `STORE_SLOT` — the mutation path through the new `+` operator |
| 14 | `save` | wrote the image to persistent storage |
| 15 | `clear` | wiped the runtime |
| 16 | `counter` | `err 7` — counter is gone |
| 17 | `greeting` | `err 7` — greeting is gone |
| 18 | `bump:` | `err 7` — bump is gone |
| 19 | `restore` | read the saved image back |
| 20 | `counter` | `3` — survived |
| 21 | `greeting` | `"frothy beta lives"` — survived |
| 22 | `bump:` | the restored function ran the mutator on the restored counter |
| 23 | `counter` | `4` — mutation worked after restore |
| 24 | `pad.reset` | pad is runtime state, not persisted; reset it for the next loop run |
| 25 | `fill:` | the restored function ran the loop again |
| 26 | `pad.len:` | confirmed `3` — same result as before save |

## Caveats and known gaps

The host_normal profile uses **in-memory storage** for persistence
(see `targets/host/platform.c` — `fr_platform_storage` is a static
array). The save/restore loop above proves the framing is correct, but
it only survives within a single process. The same code on the ESP32
target writes to flash and would survive a power cycle; that is the
on-device proof we owe at release time.

Drafting this transcript surfaced one real gap that was fixed on the
same branch: the `+` operator was missing from the parser. T4a added
`-`, `*`, `/` (and the comparison operators), but `+` was omitted from
the spec and so never wired into the parse tree. The `FR_OP_ADD_INT`
opcode existed at the VM level and was exercised by low-level VM tests
via hand-built bytecode, but no source ever produced an `ADD_INT`
instruction. The closeout commit on this branch adds the missing parser
token, parse-tree kind, additive-precedence handling, and compile case,
plus CHECK tests mirroring the existing subtraction coverage. The
transcript above uses the new path: `set counter to counter + 1` would
have failed with `err 8` before this commit landed.
