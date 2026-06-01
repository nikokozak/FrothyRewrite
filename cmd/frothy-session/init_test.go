package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

func TestFrothyInitWritesSkeleton(t *testing.T) {
	dir := t.TempDir()
	var stdout, stderr bytes.Buffer
	code := runInitCommand(nil, dir, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("exit %d, want 0; stderr=%q", code, stderr.String())
	}

	want := map[string]string{
		"frothy.toml": initFrothyToml,
		"main.fr":     initMainFr,
		".gitignore":  initGitignore,
	}
	for name, body := range want {
		got, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			t.Fatalf("read %s: %v", name, err)
		}
		if string(got) != body {
			t.Fatalf("%s body=%q, want %q", name, got, body)
		}
	}

	info, err := os.Stat(filepath.Join(dir, "libs"))
	if err != nil {
		t.Fatalf("stat libs: %v", err)
	}
	if !info.IsDir() {
		t.Fatalf("libs is not a directory")
	}
}

func TestFrothyInitRefusesWhenAnyTargetExists(t *testing.T) {
	for _, existing := range []string{"frothy.toml", "main.fr", ".gitignore", "libs"} {
		t.Run(existing, func(t *testing.T) {
			dir := t.TempDir()
			path := filepath.Join(dir, existing)
			if existing == "libs" {
				if err := os.Mkdir(path, 0o755); err != nil {
					t.Fatal(err)
				}
			} else {
				if err := os.WriteFile(path, []byte("preexisting"), 0o644); err != nil {
					t.Fatal(err)
				}
			}

			var stdout, stderr bytes.Buffer
			code := runInitCommand(nil, dir, &stdout, &stderr)
			if code == 0 {
				t.Fatalf("exit 0, want non-zero; refusal expected because %s existed", existing)
			}
			if !bytes.Contains(stderr.Bytes(), []byte(existing+" already exists")) {
				t.Fatalf("stderr=%q does not name %s", stderr.String(), existing)
			}

			// No other init files should have been created.
			for _, sibling := range []string{"frothy.toml", "main.fr", ".gitignore", "libs"} {
				if sibling == existing {
					continue
				}
				if _, err := os.Stat(filepath.Join(dir, sibling)); err == nil {
					t.Fatalf("init wrote %s despite refusal; should be all-or-nothing", sibling)
				}
			}
		})
	}
}

func TestFrothyInitRejectsPositionalArgs(t *testing.T) {
	dir := t.TempDir()
	var stdout, stderr bytes.Buffer
	code := runInitCommand([]string{"extra"}, dir, &stdout, &stderr)
	if code != 2 {
		t.Fatalf("exit %d, want 2 for usage error; stderr=%q", code, stderr.String())
	}
}
