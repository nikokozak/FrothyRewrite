#!/usr/bin/env bash
set -euo pipefail

# Runs the host-tagged examples through the host_normal REPL and asserts their
# inline `-- => VALUE` expectations. This is a smoke gate: it must fail loudly
# rather than pass leniently.

binary=${FROTHY_EXAMPLES_BINARY:-build/host/frothy-host-normal}

if [ ! -x "$binary" ]; then
  printf 'missing host binary: %s\n' "$binary" >&2
  printf 'run: make host-normal\n' >&2
  exit 2
fi

fail() {
  printf 'FAIL %s\n%s\n' "$1" "$2" >&2
  if [ "$#" -ge 3 ]; then
    printf '\nfull output:\n%s\n' "$3" >&2
  fi
  exit 1
}

shopt -s nullglob
count=0
skipped=0

for file in examples/*.fr; do
  # Collect @tag tokens (one per line). Every example must declare at least one
  # recognized tag; a missing or unknown tag is a hard error, not a silent skip,
  # so a file can never fall out of the gate unnoticed.
  tags=$(awk '/^--[[:space:]]*@tag[[:space:]]/ { for (i = 3; i <= NF; i++) print $i }' "$file")
  if [ -z "$tags" ]; then
    fail "$file" "no '-- @tag ...' header (every example must declare host, device, or hardware)"
  fi
  run_host=0
  while IFS= read -r t; do
    [ -n "$t" ] || continue
    case "$t" in
      host) run_host=1 ;;
      device | hardware) ;;
      *) fail "$file" "unknown tag: $t (allowed: host, device, hardware)" ;;
    esac
  done <<EOF
$tags
EOF

  if [ "$run_host" -eq 0 ]; then
    printf 'skip %s (%s)\n' "$(basename "$file")" "$(printf '%s' "$tags" | tr '\n' ' ')"
    skipped=$((skipped + 1))
    continue
  fi

  # Strip full-line comments and blank lines before feeding the REPL, the same
  # way `frothy send` delivers a file to a board. The raw REPL rejects a
  # comment-only line as invalid (error 8); trailing `-- => V` comments on code lines
  # are fine and stay.
  program=$(grep -v -E '^[[:space:]]*(--|$)' "$file" || true)
  if ! output=$(printf '%s\n' "$program" | "$binary" 2>&1); then
    fail "$file" "host runner exited non-zero" "$output"
  fi
  # The host REPL prefixes each response with '> ', errors included
  # (`> error: not found (7)`), so match the prefix, not a bare line start.
  if printf '%s\n' "$output" | grep -qE '^(> )?error:'; then
    fail "$file" "REPL output contained an error" "$output"
  fi

  # Count-based expectation check. For each distinct expected value V, the
  # transcript must carry at least as many `> V` whole-line responses as there
  # are `-- => V` expectations in the source. A broken line can no longer be
  # masked by a different line that happens to echo the same value.
  expected=$(grep -o -- '-- => .*' "$file" | sed 's/^-- => //; s/[[:space:]]*$//' || true)
  if [ -n "$expected" ]; then
    distinct=$(printf '%s\n' "$expected" | sort -u)
    while IFS= read -r v; do
      [ -n "$v" ] || continue
      want=$(printf '%s\n' "$expected" | grep -cxF "$v" || true)
      got=$(printf '%s\n' "$output" | grep -cxF "> $v" || true)
      if [ "$got" -lt "$want" ]; then
        fail "$file" "expected $want response(s) of '> $v', saw $got" "$output"
      fi
    done <<EOF
$distinct
EOF
  fi

  printf 'ok  %s\n' "$(basename "$file")"
  count=$((count + 1))
done

printf 'examples ok (%d host files, %d skipped)\n' "$count" "$skipped"
