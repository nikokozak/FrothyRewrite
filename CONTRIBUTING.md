# Contributing to Frothy

Frothy is small on purpose. Good contributions keep the system easier to read,
build, and explain.

## Getting Set Up

Clone the repo and build the local CLI:

```sh
git clone https://github.com/nikokozak/FrothyRewrite
cd FrothyRewrite
make cli
```

Then use the CLI to install and check the host tools:

```sh
frothy bootstrap
frothy doctor
```

`frothy bootstrap` installs ESP-IDF under your user account; do not use `sudo`.
See `README.md` for the full walkthrough.

## Running The Checks

Run the suites that cover the surface you changed:

```sh
make test
make test-unity
make test-host-normal-transcript
make test-esp32-plain-host-transcript
make test-lib-e2e
go test ./cmd/... ./internal/...
(cd libs/frothy-repl && npm test)
(cd libs/frothy-editor && npm test)
```

CI runs all of these on every pull request, plus an ESP32 firmware build.

## What A Good PR Looks Like

Keep it small and focused. Tests should ride with the change.

Match the surrounding style. Prefer one plain purpose per function, clear data
shape before behavior, and names a stranger can read without project history.

If a change touches hardware behavior, say which board you used and how you
verified it.

## Where To Talk

Use GitHub issues for bugs, feature requests, and design questions.
