package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writeFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

// Pure-modules library: no lib.toml, just lib.fr. Should resolve to a
// single resolvedLibrary with the directory name and no extension /
// natives.
func TestResolveDeps_PureModulesPathDep(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "libs/servo/lib.fr"), "to servo.attach [ ]\n")

	proj := projectManifest{
		Name:   "blink",
		Target: "host",
		Deps: map[string]manifestDep{
			"servo": {Path: "libs/servo"},
		},
	}
	libs, err := resolveDeps(dir, proj)
	if err != nil {
		t.Fatalf("resolveDeps: %v", err)
	}
	if len(libs) != 1 || libs[0].name != "servo" {
		t.Fatalf("got %+v", libs)
	}
	if libs[0].extension != nil || len(libs[0].natives) != 0 {
		t.Fatalf("pure-modules library should have no extension/natives")
	}
}

// Mixed library: lib.toml with [extension] + [[natives]]. The walker
// populates extension sources and natives onto the resolvedLibrary.
func TestResolveDeps_MixedLibrary(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "libs/neopixel/lib.fr"), "to neopixel.use [ ]\n")
	writeFile(t, filepath.Join(dir, "libs/neopixel/native/neopixel.c"), "/* placeholder */\n")
	writeFile(t, filepath.Join(dir, "libs/neopixel/lib.toml"), `name = "neopixel"
targets = ["host", "esp32_devkit_v1"]

[extension]
sources = ["native/neopixel.c"]

[[natives]]
name = "neopixel.show"
arity = 1
c_function = "fr_lib_neopixel_show"
`)
	proj := projectManifest{
		Name:   "stage",
		Target: "host",
		Deps:   map[string]manifestDep{"neopixel": {Path: "libs/neopixel"}},
	}
	libs, err := resolveDeps(dir, proj)
	if err != nil {
		t.Fatalf("resolveDeps: %v", err)
	}
	if len(libs) != 1 {
		t.Fatalf("expected 1 library, got %d", len(libs))
	}
	lib := libs[0]
	if lib.extension == nil || len(lib.extension.sources) != 1 {
		t.Fatalf("extension not populated: %+v", lib.extension)
	}
	if len(lib.natives) != 1 || lib.natives[0].name != "neopixel.show" {
		t.Fatalf("natives not populated: %+v", lib.natives)
	}
}

// Library-to-library dep: stage depends on servo. Resolver order must
// place servo before stage so the build's overlay compile sees servo's
// words first.
func TestResolveDeps_LibraryDep(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "libs/servo/lib.fr"), "to servo.attach [ ]\n")
	writeFile(t, filepath.Join(dir, "libs/stage/lib.fr"), "to stage.go [ ]\n")
	writeFile(t, filepath.Join(dir, "libs/stage/lib.toml"), `name = "stage"
targets = ["host"]

[deps]
servo = { path = "../servo" }
`)
	proj := projectManifest{
		Name:   "show",
		Target: "host",
		Deps:   map[string]manifestDep{"stage": {Path: "libs/stage"}},
	}
	libs, err := resolveDeps(dir, proj)
	if err != nil {
		t.Fatalf("resolveDeps: %v", err)
	}
	if len(libs) != 2 || libs[0].name != "servo" || libs[1].name != "stage" {
		t.Fatalf("dep order wrong: %+v", libs)
	}
}

// Cycle: stage -> servo -> stage. Must fail with "library dependency cycle:".
func TestResolveDeps_Cycle(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "libs/servo/lib.fr"), "")
	writeFile(t, filepath.Join(dir, "libs/servo/lib.toml"), `name = "servo"
targets = ["host"]

[deps]
stage = { path = "../stage" }
`)
	writeFile(t, filepath.Join(dir, "libs/stage/lib.fr"), "")
	writeFile(t, filepath.Join(dir, "libs/stage/lib.toml"), `name = "stage"
targets = ["host"]

[deps]
servo = { path = "../servo" }
`)
	proj := projectManifest{
		Name:   "show",
		Target: "host",
		Deps:   map[string]manifestDep{"stage": {Path: "libs/stage"}},
	}
	_, err := resolveDeps(dir, proj)
	if err == nil {
		t.Fatal("expected cycle error")
	}
	if !strings.Contains(err.Error(), "library dependency cycle") {
		t.Fatalf("wrong error: %v", err)
	}
}

func TestResolveDeps_GitDepNotFetched(t *testing.T) {
	dir := t.TempDir()
	t.Setenv("FROTH_HOME", t.TempDir())
	proj := projectManifest{
		Name:   "blink",
		Target: "host",
		Deps:   map[string]manifestDep{"servo": {Git: "https://example/x", Rev: "abc123"}},
	}
	_, err := resolveDeps(dir, proj)
	if err == nil || !strings.Contains(err.Error(), "git dep not fetched; run frothy fetch") {
		t.Fatalf("wrong error: %v", err)
	}
}

// Missing lib.fr: a directory with no lib.fr can't be a library.
func TestResolveDeps_MissingLibFr(t *testing.T) {
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "libs/empty"), 0o755); err != nil {
		t.Fatal(err)
	}
	proj := projectManifest{
		Name:   "blink",
		Target: "host",
		Deps:   map[string]manifestDep{"empty": {Path: "libs/empty"}},
	}
	_, err := resolveDeps(dir, proj)
	if err == nil || !strings.Contains(err.Error(), "lib.fr missing") {
		t.Fatalf("wrong error: %v", err)
	}
}

// Directory name must match lib.toml name.
func TestResolveDeps_DirNameMismatch(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "libs/servo/lib.fr"), "")
	writeFile(t, filepath.Join(dir, "libs/servo/lib.toml"), `name = "other"
targets = ["host"]
`)
	proj := projectManifest{
		Name:   "blink",
		Target: "host",
		Deps:   map[string]manifestDep{"servo": {Path: "libs/servo"}},
	}
	_, err := resolveDeps(dir, proj)
	if err == nil || !strings.Contains(err.Error(), "does not match directory") {
		t.Fatalf("wrong error: %v", err)
	}
}

// targetGateLibraries: target not in lib.toml's targets list fails with
// the SPEC-specified message.
func TestTargetGateLibraries_Mismatch(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "libs/neopixel/lib.fr"), "")
	writeFile(t, filepath.Join(dir, "libs/neopixel/lib.toml"), `name = "neopixel"
targets = ["esp32_devkit_v1"]

[extension]
sources = ["native/x.c"]
`)
	writeFile(t, filepath.Join(dir, "libs/neopixel/native/x.c"), "")
	libs := []resolvedLibrary{{name: "neopixel", path: filepath.Join(dir, "libs/neopixel")}}
	err := targetGateLibraries("atmega328p", libs, dir)
	if err == nil {
		t.Fatal("expected target gate failure")
	}
	want := "library neopixel does not support target atmega328p"
	if !strings.Contains(err.Error(), want) {
		t.Fatalf("got %q, want it to contain %q", err.Error(), want)
	}
}

// targetGateLibraries: pure-modules library (no lib.toml) is always
// allowed.
func TestTargetGateLibraries_PureModulesAlwaysAllowed(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "libs/helpers/lib.fr"), "")
	libs := []resolvedLibrary{{name: "helpers", path: filepath.Join(dir, "libs/helpers")}}
	if err := targetGateLibraries("atmega328p", libs, dir); err != nil {
		t.Fatalf("pure-modules library should not gate: %v", err)
	}
}
