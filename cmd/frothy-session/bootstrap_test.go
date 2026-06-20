package main

import (
	"bytes"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestBootstrapPassesForceFlagThrough verifies the verb wires --force into
// the script's argv. User-model corruption it prevents: a user who passes
// --force expecting a reinstall and gets the idempotent no-op instead.
func TestBootstrapPassesForceFlagThrough(t *testing.T) {
	withTempScript(t)
	var got []string
	runner := func(path string, args []string, stdout io.Writer, stderr io.Writer) int {
		got = args
		return 0
	}
	var stdout, stderr bytes.Buffer
	if code := runBootstrapCommand([]string{"--force"}, &stdout, &stderr, runner); code != 0 {
		t.Fatalf("bootstrap --force exit = %d, want 0; stderr=%q", code, stderr.String())
	}
	if len(got) != 1 || got[0] != "--force" {
		t.Errorf("script argv = %v, want [\"--force\"]", got)
	}
}

// TestBootstrapPropagatesScriptExitCode verifies the verb returns whatever
// the script returns. User-model corruption it prevents: a silent success
// after the script actually failed (or vice versa).
func TestBootstrapPropagatesScriptExitCode(t *testing.T) {
	withTempScript(t)
	runner := func(path string, args []string, stdout io.Writer, stderr io.Writer) int {
		return 7
	}
	var stdout, stderr bytes.Buffer
	if code := runBootstrapCommand(nil, &stdout, &stderr, runner); code != 7 {
		t.Errorf("bootstrap exit = %d, want 7 (the script's code)", code)
	}
}

// TestBootstrapFailsClearlyWithoutScript verifies the verb refuses to run
// when the script is missing and points the user at the fix. User-model
// corruption it prevents: a cryptic "no such file" error when the user
// runs bootstrap from outside the repo root.
func TestBootstrapFailsClearlyWithoutScript(t *testing.T) {
	withTempDir(t)
	runner := func(path string, args []string, stdout io.Writer, stderr io.Writer) int {
		t.Errorf("runner should not be called when script is missing")
		return 0
	}
	var stdout, stderr bytes.Buffer
	if code := runBootstrapCommand(nil, &stdout, &stderr, runner); code != 2 {
		t.Errorf("bootstrap exit = %d, want 2", code)
	}
	if !strings.Contains(stderr.String(), "not found") {
		t.Errorf("stderr = %q, want a \"not found\" message", stderr.String())
	}
	if !strings.Contains(stderr.String(), "repo root") {
		t.Errorf("stderr = %q, want a \"repo root\" hint", stderr.String())
	}
}

// withTempScript cd's the test into a temp directory containing a stub
// tools/setup-esp-idf.sh so runBootstrapCommand's file-exists check passes.
func withTempScript(t *testing.T) {
	t.Helper()
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "tools"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "tools", "setup-esp-idf.sh"), []byte("#!/bin/sh\nexit 0\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	cwd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Chdir(dir); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chdir(cwd) })
}

// withTempDir cd's the test into an empty temp directory so the verb's
// file-exists check fails.
func withTempDir(t *testing.T) {
	t.Helper()
	dir := t.TempDir()
	cwd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Chdir(dir); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chdir(cwd) })
}
