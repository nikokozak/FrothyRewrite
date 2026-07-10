package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

type menuCall struct {
	verb        string
	args        []string
	interactive bool
}

func recordingMenuRunner(fail map[string]int, calls *[]menuCall) menuVerbRunner {
	return func(verb string, args []string, interactive bool) int {
		*calls = append(*calls, menuCall{
			verb:        verb,
			args:        append([]string(nil), args...),
			interactive: interactive,
		})
		if code, ok := fail[verb]; ok {
			return code
		}
		return 0
	}
}

func TestMenuSetupBoardRoutesThroughExistingVerbs(t *testing.T) {
	ctx := menuContext{
		canonical: true,
		boards:    []string{"esp32_devkit_v1", "other_board"},
	}
	var out, errBuf bytes.Buffer
	var calls []menuCall

	code := runMenuCommand(nil, strings.NewReader("1\n1\n"), &out, &errBuf, ctx, recordingMenuRunner(nil, &calls))

	if code != 0 {
		t.Fatalf("menu exit = %d, want 0\nstderr:\n%s\nstdout:\n%s", code, errBuf.String(), out.String())
	}
	want := []menuCall{
		{verb: "doctor"},
		{verb: "flash", args: []string{"esp32_devkit_v1"}},
		{verb: "connect", interactive: true},
	}
	assertMenuCalls(t, calls, want)
}

func TestMenuProjectSetupBuildsInstallsThenConnects(t *testing.T) {
	ctx := menuContext{inProject: true}
	var out, errBuf bytes.Buffer
	var calls []menuCall

	code := runMenuCommand(nil, strings.NewReader("1\n"), &out, &errBuf, ctx, recordingMenuRunner(nil, &calls))

	if code != 0 {
		t.Fatalf("menu exit = %d, want 0\nstderr:\n%s\nstdout:\n%s", code, errBuf.String(), out.String())
	}
	want := []menuCall{
		{verb: "build"},
		{verb: "install"},
		{verb: "connect", interactive: true},
	}
	assertMenuCalls(t, calls, want)
}

func TestMenuFlashFailureDoesNotConnect(t *testing.T) {
	ctx := menuContext{canonical: true, boards: []string{"esp32_devkit_v1"}}
	var out, errBuf bytes.Buffer
	var calls []menuCall

	code := runMenuCommand(nil, strings.NewReader("1\n\n"), &out, &errBuf, ctx, recordingMenuRunner(map[string]int{"flash": 1}, &calls))

	if code != 1 {
		t.Fatalf("menu exit = %d, want 1\nstderr:\n%s\nstdout:\n%s", code, errBuf.String(), out.String())
	}
	want := []menuCall{
		{verb: "doctor"},
		{verb: "flash", args: []string{"esp32_devkit_v1"}},
	}
	assertMenuCalls(t, calls, want)
	if !strings.Contains(out.String(), "Recovery options:") {
		t.Fatalf("stdout = %q, want recovery options after flash failure", out.String())
	}
}

func TestMenuRecoveryStopRoutesToStop(t *testing.T) {
	ctx := menuContext{canonical: true, boards: []string{"esp32_devkit_v1"}}
	var out, errBuf bytes.Buffer
	var calls []menuCall

	code := runMenuCommand(nil, strings.NewReader("3\n1\n"), &out, &errBuf, ctx, recordingMenuRunner(nil, &calls))

	if code != 0 {
		t.Fatalf("menu exit = %d, want 0\nstderr:\n%s\nstdout:\n%s", code, errBuf.String(), out.String())
	}
	want := []menuCall{{verb: "stop"}}
	assertMenuCalls(t, calls, want)
}

func TestMenuRecoveryTypoReprompts(t *testing.T) {
	ctx := menuContext{canonical: true, boards: []string{"esp32_devkit_v1"}}
	var out, errBuf bytes.Buffer
	var calls []menuCall

	code := runMenuCommand(nil, strings.NewReader("3\nx\n1\n"), &out, &errBuf, ctx, recordingMenuRunner(nil, &calls))

	if code != 0 {
		t.Fatalf("menu exit = %d, want 0\nstderr:\n%s\nstdout:\n%s", code, errBuf.String(), out.String())
	}
	want := []menuCall{{verb: "stop"}}
	assertMenuCalls(t, calls, want)
	if !strings.Contains(out.String(), "Choose 1, 2, 3, 4, 5, b, or q.") {
		t.Fatalf("stdout = %q, want typo guidance", out.String())
	}
}

func TestMenuRecoveryBackReturnsToHome(t *testing.T) {
	ctx := menuContext{canonical: true, boards: []string{"esp32_devkit_v1"}}
	var out, errBuf bytes.Buffer
	var calls []menuCall

	code := runMenuCommand(nil, strings.NewReader("3\nb\nq\n"), &out, &errBuf, ctx, recordingMenuRunner(nil, &calls))

	if code != 0 {
		t.Fatalf("menu exit = %d, want 0\nstderr:\n%s\nstdout:\n%s", code, errBuf.String(), out.String())
	}
	if len(calls) != 0 {
		t.Fatalf("calls = %#v, want none", calls)
	}
	if strings.Count(out.String(), "Frothy\n\n  1)") != 2 {
		t.Fatalf("stdout = %q, want home menu before and after back", out.String())
	}
}

func TestMenuRunnerUsesCanonicalRootAsChildDirectory(t *testing.T) {
	repoRoot := t.TempDir()
	logPath := filepath.Join(t.TempDir(), "cwd.txt")
	script := filepath.Join(t.TempDir(), "frothy")
	if err := os.WriteFile(script, []byte("#!/bin/sh\npwd > \"$1\"\n"), 0o755); err != nil {
		t.Fatal(err)
	}

	ctx := menuContext{canonical: true, repoRoot: repoRoot, selfPath: script}
	if code := ctx.runVerb(logPath, nil, false); code != 0 {
		t.Fatalf("runVerb exit = %d, want 0", code)
	}
	got, err := os.ReadFile(logPath)
	if err != nil {
		t.Fatal(err)
	}
	if strings.TrimSpace(string(got)) != repoRoot {
		t.Fatalf("child cwd = %q, want %q", strings.TrimSpace(string(got)), repoRoot)
	}
}

func TestResolveMenuExecutableMakesRelativePathAbsolute(t *testing.T) {
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	got := resolveMenuExecutable("./frothy")
	want := filepath.Join(wd, "frothy")
	if got != want {
		t.Fatalf("resolveMenuExecutable = %q, want %q", got, want)
	}
}

func TestFlashableBoardUsesManifestTarget(t *testing.T) {
	boardsDir := t.TempDir()
	writeTestBoard(t, boardsDir, "esp32", `{"target":"esp-idf"}`)
	writeTestBoard(t, boardsDir, "host", `{"target":"host"}`)
	writeTestBoard(t, boardsDir, "malformed", `{`)

	for _, test := range []struct {
		id   string
		want bool
	}{
		{id: "esp32", want: true},
		{id: "host"},
		{id: "malformed"},
		{id: "missing"},
		{id: "../esp32"},
	} {
		t.Run(test.id, func(t *testing.T) {
			if got := flashableBoard(boardsDir, test.id); got != test.want {
				t.Fatalf("flashableBoard(%q) = %v, want %v", test.id, got, test.want)
			}
		})
	}
}

func TestMenuBoardListExcludesHostAndMalformedManifests(t *testing.T) {
	root := makeSourceRoot(t)
	boardsDir := filepath.Join(root, "boards")
	writeTestBoard(t, boardsDir, "esp32_devkit_v1", `{"target":"esp-idf"}`)
	writeTestBoard(t, boardsDir, "host", `{"target":"host"}`)
	writeTestBoard(t, boardsDir, "broken", `{`)
	t.Setenv(frothySourceRootEnv, root)

	if got := strings.Join(defaultMenuContext().boards, ","); got != "esp32_devkit_v1" {
		t.Fatalf("flashable boards = %q, want esp32_devkit_v1", got)
	}
}

func writeTestBoard(t *testing.T, boardsDir, id, manifest string) {
	t.Helper()
	dir := filepath.Join(boardsDir, id)
	if err := os.Mkdir(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "board.json"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}
}

func assertMenuCalls(t *testing.T, got []menuCall, want []menuCall) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("calls = %#v, want %#v", got, want)
	}
	for i := range want {
		if got[i].verb != want[i].verb || got[i].interactive != want[i].interactive ||
			strings.Join(got[i].args, "\x00") != strings.Join(want[i].args, "\x00") {
			t.Fatalf("calls = %#v, want %#v", got, want)
		}
	}
}
