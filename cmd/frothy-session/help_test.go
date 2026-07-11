package main

import (
	"bytes"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// checkGolden compares got against testdata/help/<verb>.txt. Run with
// UPDATE_GOLDEN=1 to rewrite the golden after a deliberate help-text change.
// Invariant: help text changes are deliberate, not accidental drift.
func checkGolden(t *testing.T, verb string, got []byte) {
	t.Helper()
	path := filepath.Join("testdata", "help", verb+".txt")
	if os.Getenv("UPDATE_GOLDEN") == "1" {
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, got, 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	want, err := os.ReadFile(path)
	if err != nil {
		t.Errorf("no golden for %q at %s; rerun with UPDATE_GOLDEN=1 to create it\n--- got ---\n%s", verb, path, got)
		return
	}
	if !bytes.Equal(got, want) {
		t.Errorf("help for %q drifted from golden; rerun with UPDATE_GOLDEN=1 to update\n--- got ---\n%s", verb, got)
	}
}

// frothy help with no verb prints top-level usage on stdout and exits 0;
// an unknown verb reports "no such verb" on stderr and exits 2. The
// known-verb dispatch path is covered by TestHelpAliasDispatchesKnownVerb.
func TestHelpAliasRouting(t *testing.T) {
	verbs := availableVerbs()

	var out, errBuf bytes.Buffer
	if code := runFrothyCommand([]string{"frothy", "help"}, &out, &errBuf, verbs); code != 0 {
		t.Fatalf("help (no verb) exit = %d, want 0", code)
	}
	if !bytes.Contains(out.Bytes(), []byte("usage: frothy")) {
		t.Errorf("help (no verb) stdout = %q, want top-level usage", out.String())
	}

	out.Reset()
	errBuf.Reset()
	if code := runFrothyCommand([]string{"frothy", "help", "nonexistent"}, &out, &errBuf, verbs); code != 2 {
		t.Fatalf("help nonexistent exit = %d, want 2", code)
	}
	if !bytes.Contains(errBuf.Bytes(), []byte("no such verb: nonexistent")) {
		t.Errorf("help nonexistent stderr = %q, want \"no such verb\"", errBuf.String())
	}
	if !bytes.Contains(out.Bytes(), []byte("usage: frothy")) {
		t.Errorf("help nonexistent stdout = %q, want top-level usage", out.String())
	}
}

// frothy help <verb> rewrites os.Args to "<verb> --help" and dispatches to
// that verb's run function, so the alias and a direct --help share one path.
func TestHelpAliasDispatchesKnownVerb(t *testing.T) {
	savedArgs := os.Args
	defer func() { os.Args = savedArgs }()

	var gotArgs []string
	verbs := []verb{{name: "send", summary: "stub", run: func() int {
		gotArgs = os.Args
		return 0
	}}}

	var out, errBuf bytes.Buffer
	if code := runFrothyCommand([]string{"frothy", "help", "send"}, &out, &errBuf, verbs); code != 0 {
		t.Fatalf("help send exit = %d, want 0", code)
	}
	if len(gotArgs) != 2 || gotArgs[0] != "frothy send" || gotArgs[1] != "--help" {
		t.Errorf("os.Args = %q, want [\"frothy send\" \"--help\"]", gotArgs)
	}
}

func TestSendHelpSnapshot(t *testing.T) {
	var out bytes.Buffer
	if code := runSendCommand([]string{"--help"}, &out, io.Discard); code != 0 {
		t.Fatalf("send --help exit = %d, want 0", code)
	}
	checkGolden(t, "send", out.Bytes())
}

func TestFlashHelpSnapshot(t *testing.T) {
	var out bytes.Buffer
	if code := runFlashCommand([]string{"--help"}, "", &out, io.Discard, nil, nil); code != 0 {
		t.Fatalf("flash --help exit = %d, want 0", code)
	}
	checkGolden(t, "flash", out.Bytes())
}

func TestWipeHelpSnapshot(t *testing.T) {
	var out bytes.Buffer
	if code := runWipeCommand([]string{"--help"}, "", &out, io.Discard, nil, nil); code != 0 {
		t.Fatalf("wipe --help exit = %d, want 0", code)
	}
	checkGolden(t, "wipe", out.Bytes())
}

func TestWipeUserHelpSnapshot(t *testing.T) {
	var out bytes.Buffer
	if code := runWipeUserCommand([]string{"--help"}, &out, io.Discard, nil, nil, 0, 0, 0); code != 0 {
		t.Fatalf("wipe-user --help exit = %d, want 0", code)
	}
	checkGolden(t, "wipe-user", out.Bytes())
}

func TestDoctorHelpSnapshot(t *testing.T) {
	var out bytes.Buffer
	if code := runDoctorCommand([]string{"--help"}, &out, io.Discard, nil); code != 0 {
		t.Fatalf("doctor --help exit = %d, want 0", code)
	}
	checkGolden(t, "doctor", out.Bytes())
}

func TestConnectHelpSnapshot(t *testing.T) {
	var out bytes.Buffer
	if code := runConnectCommand([]string{"--help"}, nil, &out, io.Discard, nil, nil, nil); code != 0 {
		t.Fatalf("connect --help exit = %d, want 0", code)
	}
	checkGolden(t, "connect", out.Bytes())
}

func TestStopHelpSnapshot(t *testing.T) {
	var out bytes.Buffer
	if code := runStopCommand([]string{"--help"}, &out, io.Discard, defaultSerialStopper()); code != 0 {
		t.Fatalf("stop --help exit = %d, want 0", code)
	}
	checkGolden(t, "stop", out.Bytes())
}

func TestInitHelpSnapshot(t *testing.T) {
	var out bytes.Buffer
	if code := runInitCommand([]string{"--help"}, ".", &out, io.Discard); code != 0 {
		t.Fatalf("init --help exit = %d, want 0", code)
	}
	checkGolden(t, "init", out.Bytes())
}

func TestBuildHelpSnapshot(t *testing.T) {
	var out bytes.Buffer
	if code := runBuildCommand([]string{"--help"}, &out, io.Discard); code != 0 {
		t.Fatalf("build --help exit = %d, want 0", code)
	}
	checkGolden(t, "build", out.Bytes())
}

func TestFetchHelpSnapshot(t *testing.T) {
	var out bytes.Buffer
	if code := runFetchCommand([]string{"--help"}, &out, io.Discard); code != 0 {
		t.Fatalf("fetch --help exit = %d, want 0", code)
	}
	checkGolden(t, "fetch", out.Bytes())
}

func TestInstallHelpSnapshot(t *testing.T) {
	var out bytes.Buffer
	if code := runInstallCommand([]string{"--help"}, &out, io.Discard, nil, nil, 0, 0, 0); code != 0 {
		t.Fatalf("install --help exit = %d, want 0", code)
	}
	checkGolden(t, "install", out.Bytes())
}

func TestBootstrapHelpSnapshot(t *testing.T) {
	var out bytes.Buffer
	if code := runBootstrapCommand([]string{"--help"}, &out, io.Discard, nil); code != 0 {
		t.Fatalf("bootstrap --help exit = %d, want 0", code)
	}
	checkGolden(t, "bootstrap", out.Bytes())
}

func TestMenuHelpSnapshot(t *testing.T) {
	var out bytes.Buffer
	if code := runMenuCommand([]string{"--help"}, strings.NewReader(""), &out, io.Discard, menuContext{}, nil); code != 0 {
		t.Fatalf("menu --help exit = %d, want 0", code)
	}
	checkGolden(t, "menu", out.Bytes())
}

// TestHelpShapeConsistency asserts every verb in availableVerbs() prints help
// in the documented template: a "frothy <verb> —" summary line, an "Examples:"
// section, and a "Flags:" section. User-model corruption it prevents:
// a learner internalizing one verb's help shape only to find a different shape
// on the next verb.
func TestHelpShapeConsistency(t *testing.T) {
	for _, v := range availableVerbs() {
		t.Run(v.name, func(t *testing.T) {
			path := filepath.Join("testdata", "help", v.name+".txt")
			got, err := os.ReadFile(path)
			if err != nil {
				t.Fatalf("no golden for %q at %s; rerun with UPDATE_GOLDEN=1 to create it", v.name, path)
			}
			text := string(got)
			summary := "frothy " + v.name + " — "
			if !strings.HasPrefix(text, summary) {
				t.Errorf("help for %q must start with %q", v.name, summary)
			}
			if !strings.Contains(text, "\nExamples:\n") {
				t.Errorf("help for %q must contain an Examples: section", v.name)
			}
			if !strings.Contains(text, "\nFlags:\n") {
				t.Errorf("help for %q must contain a Flags: section", v.name)
			}
		})
	}
}
