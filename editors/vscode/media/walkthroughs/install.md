## Install the Frothy CLI

The extension does not talk to your board directly — it drives the `frothy`
CLI, which owns serial discovery, flashing, and the live session.

Install a packaged build, or build from a source checkout and put `frothy`
on your `PATH`. If it lives somewhere else, point the `frothy.binaryPath`
setting at it.

Then run **Frothy: Doctor** — it checks the CLI, serial discovery, and any
connected device, and tells you what (if anything) is missing.
