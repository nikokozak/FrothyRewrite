package main

import (
	"strings"
	"testing"
)

func TestParseProjectManifestMinimum(t *testing.T) {
	src := `name = "blink"
board = "esp32_devkit_v1"
`
	got, err := parseProjectManifest([]byte(src))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got.Name != "blink" || got.Board != "esp32_devkit_v1" {
		t.Fatalf("unexpected fields: %+v", got)
	}
	if len(got.Deps) != 0 {
		t.Fatalf("expected no deps, got %d", len(got.Deps))
	}
}

func TestParseProjectManifestWithDeps(t *testing.T) {
	src := `name = "stage-lights"
board = "esp32_devkit_v1"

[deps]
servo    = { path = "libs/servo" }
neopixel = { git = "https://example/np", rev = "v0.1.0" }
`
	got, err := parseProjectManifest([]byte(src))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got.Deps["servo"].Path != "libs/servo" {
		t.Fatalf("servo path mismatch: %+v", got.Deps["servo"])
	}
	np := got.Deps["neopixel"]
	if np.Git != "https://example/np" || np.Rev != "v0.1.0" {
		t.Fatalf("neopixel mismatch: %+v", np)
	}
}

func TestParseProjectManifestRejects(t *testing.T) {
	cases := map[string]struct {
		src     string
		wantErr string
	}{
		"missing name": {
			src:     `board = "esp32_devkit_v1"`,
			wantErr: "missing name",
		},
		"missing board": {
			src:     `name = "blink"`,
			wantErr: "missing board",
		},
		"old target key": {
			src:     `name = "blink"` + "\n" + `target = "esp32_devkit_v1"`,
			wantErr: `unknown key "target"`,
		},
		"unknown top key": {
			src:     `name = "x"` + "\n" + `board = "host"` + "\n" + `boards = ["esp32"]`,
			wantErr: "unknown key",
		},
		"dep with neither path nor git": {
			src: `name = "x"
board = "host"

[deps]
foo = { rev = "v1" }`,
			wantErr: "must declare path or git",
		},
		"dep with both path and git": {
			src: `name = "x"
board = "host"

[deps]
foo = { path = "libs/foo", git = "https://example/foo" }`,
			wantErr: "declares both path and git",
		},
		"path dep with rev": {
			src: `name = "x"
board = "host"

[deps]
foo = { path = "libs/foo", rev = "v1" }`,
			wantErr: "path dep cannot have rev or branch",
		},
		"git dep without rev": {
			src: `name = "x"
board = "host"

[deps]
foo = { git = "https://example/foo" }`,
			wantErr: "git dep must pin rev",
		},
		"git dep with branch": {
			src: `name = "x"
board = "host"

[deps]
foo = { git = "https://example/foo", branch = "main" }`,
			wantErr: "git dep must pin rev, not branch",
		},
		"malformed toml": {
			src:     `name = `,
			wantErr: "frothy.toml:",
		},
	}
	for name, tc := range cases {
		t.Run(name, func(t *testing.T) {
			_, err := parseProjectManifest([]byte(tc.src))
			if err == nil {
				t.Fatalf("expected error containing %q, got nil", tc.wantErr)
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Fatalf("error %q does not contain %q", err.Error(), tc.wantErr)
			}
		})
	}
}

func TestParseLibraryManifestPureWithDeps(t *testing.T) {
	src := `name = "stage"
version = "0.1.0"
boards = ["esp32_devkit_v1"]

[deps]
servo = { path = "../servo" }
`
	got, err := parseLibraryManifest([]byte(src))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got.Name != "stage" || got.Version != "0.1.0" {
		t.Fatalf("unexpected fields: %+v", got)
	}
	if len(got.Boards) != 1 || got.Boards[0] != "esp32_devkit_v1" {
		t.Fatalf("unexpected boards: %+v", got.Boards)
	}
	if got.Extension != nil {
		t.Fatalf("expected no extension, got %+v", got.Extension)
	}
	if got.Deps["servo"].Path != "../servo" {
		t.Fatalf("servo dep mismatch: %+v", got.Deps["servo"])
	}
}

func TestParseLibraryManifestMixed(t *testing.T) {
	src := `name    = "neopixel"
version = "0.1.0"
boards = ["host", "esp32_devkit_v1"]

[extension]
sources = ["native/neopixel.c"]

[[natives]]
name = "neopixel.show"
arity = 1
c_function = "fr_lib_neopixel_show"

[[natives]]
name = "neopixel.set"
arity = 5
c_function = "fr_lib_neopixel_set"
`
	got, err := parseLibraryManifest([]byte(src))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got.Extension == nil || len(got.Extension.Sources) != 1 {
		t.Fatalf("extension mismatch: %+v", got.Extension)
	}
	if got.Extension.Sources[0] != "native/neopixel.c" {
		t.Fatalf("sources[0] = %q", got.Extension.Sources[0])
	}
	if len(got.Natives) != 2 {
		t.Fatalf("expected 2 natives, got %d", len(got.Natives))
	}
	if got.Natives[0].Name != "neopixel.show" || got.Natives[0].Arity != 1 {
		t.Fatalf("natives[0] mismatch: %+v", got.Natives[0])
	}
	if got.Natives[1].CFunction != "fr_lib_neopixel_set" {
		t.Fatalf("natives[1].CFunction = %q", got.Natives[1].CFunction)
	}
}

func TestParseLibraryManifestRejects(t *testing.T) {
	cases := map[string]struct {
		src     string
		wantErr string
	}{
		"missing name": {
			src:     `boards = ["host"]`,
			wantErr: "missing name",
		},
		"missing boards": {
			src:     `name = "x"`,
			wantErr: "missing boards",
		},
		"empty boards": {
			src: `name = "x"
boards = []`,
			wantErr: "missing boards",
		},
		"old targets key": {
			src: `name = "x"
targets = ["host"]`,
			wantErr: `unknown key "targets"`,
		},
		"unknown top key": {
			src: `name = "x"
boards = ["host"]
authors = ["me"]`,
			wantErr: "unknown key",
		},
		"extension without sources": {
			src: `name = "x"
boards = ["host"]
[extension]`,
			wantErr: "[extension] declared without sources",
		},
		"natives without extension": {
			src: `name = "x"
boards = ["host"]

[[natives]]
name = "x.do"
arity = 0
c_function = "fr_lib_x_do"`,
			wantErr: "[[natives]] without [extension]",
		},
		"native missing name": {
			src: `name = "x"
boards = ["host"]

[extension]
sources = ["native/x.c"]

[[natives]]
arity = 0
c_function = "fr_lib_x_do"`,
			wantErr: "native #1 missing name",
		},
		"native missing c_function": {
			src: `name = "x"
boards = ["host"]

[extension]
sources = ["native/x.c"]

[[natives]]
name = "x.do"
arity = 0`,
			wantErr: "missing c_function",
		},
		"native arity too big": {
			src: `name = "x"
boards = ["host"]

[extension]
sources = ["native/x.c"]

[[natives]]
name = "x.do"
arity = 999
c_function = "fr_lib_x_do"`,
			wantErr: "arity 999 out of range",
		},
		"native arity negative": {
			src: `name = "x"
boards = ["host"]

[extension]
sources = ["native/x.c"]

[[natives]]
name = "x.do"
arity = -1
c_function = "fr_lib_x_do"`,
			wantErr: "arity -1 out of range",
		},
		"dep both path and git": {
			src: `name = "x"
boards = ["host"]

[deps]
y = { path = "../y", git = "https://example/y" }`,
			wantErr: "declares both path and git",
		},
		"git dep with branch": {
			src: `name = "x"
boards = ["host"]

[deps]
y = { git = "https://example/y", branch = "main" }`,
			wantErr: "git dep must pin rev, not branch",
		},
	}
	for name, tc := range cases {
		t.Run(name, func(t *testing.T) {
			_, err := parseLibraryManifest([]byte(tc.src))
			if err == nil {
				t.Fatalf("expected error containing %q, got nil", tc.wantErr)
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Fatalf("error %q does not contain %q", err.Error(), tc.wantErr)
			}
		})
	}
}

func TestParseLibraryManifestRequiresAndMetadata(t *testing.T) {
	m, err := parseLibraryManifest([]byte(`name = "icp10125"
description = "ICP-10125 barometric sensor"
label = "Pressure sensor"
boards = ["esp32_devkit_v1"]
requires = ["i2c", "cells"]
`))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if m.Description != "ICP-10125 barometric sensor" || m.Label != "Pressure sensor" {
		t.Fatalf("metadata lost: %+v", m)
	}
	if len(m.Requires) != 2 || m.Requires[0] != "i2c" || m.Requires[1] != "cells" {
		t.Fatalf("requires lost: %+v", m.Requires)
	}
}

func TestParseLibraryManifestRejectsUnknownRequires(t *testing.T) {
	_, err := parseLibraryManifest([]byte(`name = "x"
boards = ["esp32_devkit_v1"]
requires = ["warp"]
`))
	if err == nil || !strings.Contains(err.Error(), `unknown capability "warp"`) {
		t.Fatalf("want unknown-capability requires error, got %v", err)
	}
}

func TestCapabilityGateLibraries(t *testing.T) {
	libs := []resolvedLibrary{
		{name: "stepper", requires: []string{"cells"}}, // always-on: ok
		{name: "beacon", requires: []string{"ble"}},    // offered, may be off
	}

	// No deviations: ble defaults on, so the ble requirement is satisfied.
	if err := capabilityGateLibraries(nil, libs); err != nil {
		t.Fatalf("default composition must satisfy all requires: %v", err)
	}

	// ble explicitly disabled: beacon's requirement is now violated.
	err := capabilityGateLibraries(map[string]bool{"ble": false}, libs)
	if err == nil || !strings.Contains(err.Error(),
		`library beacon requires capability "ble", which is disabled`) {
		t.Fatalf("want ble-disabled requires failure, got %v", err)
	}

	// ble explicitly true (absence-vs-false trap): still satisfied.
	if err := capabilityGateLibraries(map[string]bool{"ble": true}, libs); err != nil {
		t.Fatalf("ble=true must satisfy the requirement: %v", err)
	}
}
