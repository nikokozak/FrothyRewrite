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

CI runs all of these on every pull request, plus both official ESP-IDF board
builds.

## What A Good PR Looks Like

Keep it small and focused. Tests should ride with the change.

Match the surrounding style. Prefer one plain purpose per function, clear data
shape before behavior, and names a stranger can read without project history.

If a change touches hardware behavior, say which board you used and how you
verified it.

## Releasing And Vendored Bundles

The website (frothy.dev) serves two things built from this repo: the web
flasher's firmware segments and the browser editor bundle. Both are vendored —
built here, copied into the site — so the site stays self-contained and works
offline. Firmware files carry embedded build timestamps and are never
byte-identical between builds, so "update it" always means "rebuild and
re-vendor", never "diff the bytes".

**Version scheme.** `vX.Y.Z` git tags are the project release. Components keep
their own versions (the VS Code extension and `@frothy/editor` each have their
own `package.json` version); a release bundles a coherent set of them.

**Update the flasher firmware** on a site checkout (needs ESP-IDF once, via
`frothy bootstrap`):

```sh
tools/build-flasher-bundle.sh ~/Developer/frothy-site/static/test/flash/firmware
```

This discovers every official ESP-IDF board, runs its normal build, copies only
the files listed by ESP-IDF's generated `flasher_args.json`, and writes their
addresses plus the build version (from `git describe`) to one flasher
`manifest.json`.

**Update the editor bundle** the same way:

```sh
tools/build-editor-bundle.sh ~/Developer/frothy-site/static/test/editor
```

Then point the page's import at the printed version and drop the old vendored
directory.

**Cut a release.** Update `CHANGELOG.md`, then tag and push:

```sh
git tag v0.1.0
git push origin v0.1.0
```

The push triggers `.github/workflows/release.yml`, which builds the same
segmented flasher bundle and `make vsix`, then attaches the manifest, firmware
segments, and `.vsix` to a GitHub release. Re-vendor the site bundles from the
tagged commit so the live site matches the release.

**Publish the CLI through Homebrew** only after that tag archive is reachable.
Render the proven formula template with the real release values:

```sh
version=X.Y.Z
url="https://github.com/nikokozak/FrothyRewrite/archive/refs/tags/v${version}.tar.gz"
firmware_url="https://github.com/nikokozak/FrothyRewrite/releases/download/v${version}/frothy-firmware-v${version}.tar.gz"
curl -fL "$url" -o "/tmp/frothy-${version}.tar.gz"
curl -fL "$firmware_url" -o "/tmp/frothy-firmware-${version}.tar.gz"
sha256="$(shasum -a 256 "/tmp/frothy-${version}.tar.gz" | cut -d ' ' -f 1)"
firmware_sha256="$(shasum -a 256 "/tmp/frothy-firmware-${version}.tar.gz" | cut -d ' ' -f 1)"
sed -e "s|@VERSION@|${version}|g" \
    -e "s|@URL@|${url}|g" \
    -e "s|@SHA256@|${sha256}|g" \
    -e "s|@FIRMWARE_URL@|${firmware_url}|g" \
    -e "s|@FIRMWARE_SHA256@|${firmware_sha256}|g" \
    packaging/homebrew/frothy.rb.in > /tmp/frothy.rb
brew style /tmp/frothy.rb
```

Review `/tmp/frothy.rb`, then commit it as `Formula/frothy.rb` in the dedicated
tap. The package installs `frothy`, `esptool`, and the release firmware, so
`frothy flash BOARD` works without a source checkout. Firmware development and
custom firmware builds still require the Frothy source and ESP-IDF. Publishing
the tap is a separate authorized release action, never part of an ordinary code
push.

## Where To Talk

Use GitHub issues for bugs, feature requests, and design questions.
