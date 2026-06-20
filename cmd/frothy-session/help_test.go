package main

import (
	"bytes"
	"io"
	"os"
	"path/filepath"
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
// an unknown verb reports "no such verb" on stderr and exits 2. The verb
// dispatch path (frothy help <verb>) is exercised by each verb's snapshot.
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

func TestSendHelpSnapshot(t *testing.T) {
	var out bytes.Buffer
	if code := runSendCommand([]string{"--help"}, &out, io.Discard, nil); code != 0 {
		t.Fatalf("send --help exit = %d, want 0", code)
	}
	checkGolden(t, "send", out.Bytes())
}
