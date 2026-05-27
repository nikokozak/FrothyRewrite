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

1. Source-level definitions land in slots (`greeting`, `counter`, `mark`,
   `fill`).
2. `set name to expr` mutates a top-level slot via a function body
   (`mark`), and `set counter[0] to expr` mutates a cell.
3. A loop runs inside a function body (`repeat 3 [ ... ]` inside `fill`).
4. `save` persists the slot image to storage.
5. `clear` wipes the runtime — user names are gone, calls return `err 7`
   (not found).
6. `restore` reads the image back. Every user-declared slot returns to
   its saved value, including the compiled function `fill` which then
   runs the loop again and produces the same `3`.

## The transcript

Each line that starts with `>` is the REPL prompt; what follows on the
same line is the response (a value, an error code, or empty); `ok`
on the next line marks command success.

```text
> frothy status v1 profile=host_normal profile_hash=7561b535 compiler=device names=device storage=eeprom interrupt=cooperative word_size=32 int_min=-1073741824 int_max=1073741823 apply_bytes=252
ok
> ok
> "frothy beta lives"
ok
> ok
> ok
> ok
> ok
> 42
ok
> ok
> ok
> 3
ok
> 3
ok
> overlay code
PUSH_INT 3
REPEAT_BEGIN 22
PUSH_INT 65
CALL_NATIVE_SLOT 24
DROP
REPEAT_NEXT 10
PUSH_NIL
DROP
CALL_NATIVE_SLOT 25
RETURN
ok
> ok
> ok
> err 7
> err 7
> err 7
> ok
> 42
ok
> "frothy beta lives"
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
| 2  | `greeting is "frothy beta lives"` | declared a text slot |
| 3  | `greeting` | read it back |
| 4  | `counter is cells(1)` | declared a 1-cell slot |
| 5  | `set counter[0] to 0` | initialised cell |
| 6  | `mark is fn [ set counter[0] to 42 ]` | declared a function that mutates the cell |
| 7  | `mark:` | called it; cell is now 42 |
| 8  | `counter[0]` | confirmed `42` |
| 9  | `pad.reset` | cleared the pad buffer |
| 10 | `fill is fn [ repeat 3 [ pad.emit-byte: 65 ]; pad.len: ]` | declared a function that uses `repeat` to write `A` three times and returns the pad length |
| 11 | `fill:` | ran the loop; pad now has three bytes |
| 12 | `pad.len:` | confirmed `3` |
| 13 | `see fill` | inspected the compiled body, showing `REPEAT_BEGIN`, `CALL_NATIVE_SLOT`, `REPEAT_NEXT` |
| 14 | `save` | wrote the image to persistent storage |
| 15 | `clear` | wiped the runtime |
| 16 | `counter[0]` | `err 7` — counter is gone |
| 17 | `greeting` | `err 7` — greeting is gone |
| 18 | `fill:` | `err 7` — fill is gone |
| 19 | `restore` | read the saved image back |
| 20 | `counter[0]` | `42` — survived |
| 21 | `greeting` | `"frothy beta lives"` — survived |
| 22 | `pad.reset` | cleared the pad again (pad is runtime state, not persisted) |
| 23 | `fill:` | the restored function ran the loop again |
| 24 | `pad.len:` | confirmed `3` — same result as before save |

## Caveats and known gaps

The host_normal profile uses **in-memory storage** for persistence
(see `targets/host/platform.c` — `fr_platform_storage` is a static
array). The save/restore loop above proves the framing is correct, but
it only survives within a single process. The same code on the ESP32
target writes to flash and would survive a power cycle; that is the
on-device proof we owe at release time.

A second small gap surfaced while building this transcript: the
runtime apply path (`fr_compile_overlay_update_for_runtime`, used by
the REPL) does not compile arithmetic operators inside function
bodies, even though the static compiler (`fr_compile_overlay_update`,
used by the test suite) does. T4a's arithmetic op work landed and is
tested, but a function body like `[ count + 1 ]` is rejected by this
REPL with `err 8`. That gap is noted here, not fixed in this artifact;
it belongs to a follow-up runtime-compiler tranche.
