package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSourceRootEnvironmentOverride(t *testing.T) {
	root := makeSourceRoot(t)
	t.Setenv(frothySourceRootEnv, root)

	got, err := resolveFrothySourceRoot(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	if got != root {
		t.Fatalf("root = %q, want %q", got, root)
	}
}

func TestSourceRootFromStartAncestor(t *testing.T) {
	root := makeSourceRoot(t)
	start := filepath.Join(root, "projects", "blink")
	if err := os.MkdirAll(start, 0o755); err != nil {
		t.Fatal(err)
	}

	got, err := resolveFrothySourceRootFrom(start, "", "")
	if err != nil {
		t.Fatal(err)
	}
	if got != root {
		t.Fatalf("root = %q, want %q", got, root)
	}
}

func TestSourceRootFromExecutableAncestor(t *testing.T) {
	root := makeSourceRoot(t)
	executable := filepath.Join(root, "build", "host", "frothy")
	if err := os.MkdirAll(filepath.Dir(executable), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(executable, nil, 0o755); err != nil {
		t.Fatal(err)
	}

	got, err := resolveFrothySourceRootFrom(t.TempDir(), "", executable)
	if err != nil {
		t.Fatal(err)
	}
	if got != root {
		t.Fatalf("root = %q, want %q", got, root)
	}
}

func TestSourceRootRejectsInvalidOverride(t *testing.T) {
	root := makeSourceRoot(t)
	invalid := t.TempDir()

	got, err := resolveFrothySourceRootFrom(root, invalid, "")
	if err == nil || !strings.Contains(err.Error(), frothySourceRootEnv) {
		t.Fatalf("root = %q, err = %v; want invalid override error", got, err)
	}
	if got != "" {
		t.Fatalf("root = %q, want empty", got)
	}
}

func TestSourceRootResolvesSymlink(t *testing.T) {
	root := makeSourceRoot(t)
	start := filepath.Join(root, "nested")
	if err := os.Mkdir(start, 0o755); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(t.TempDir(), "frothy-link")
	if err := os.Symlink(root, link); err != nil {
		t.Fatal(err)
	}

	got, err := resolveFrothySourceRootFrom(filepath.Join(link, "nested"), "", "")
	if err != nil {
		t.Fatal(err)
	}
	if got != root {
		t.Fatalf("root = %q, want %q", got, root)
	}
}

func TestSourceRootNotFound(t *testing.T) {
	got, err := resolveFrothySourceRootFrom(t.TempDir(), "", filepath.Join(t.TempDir(), "frothy"))
	if err == nil || !strings.Contains(err.Error(), "firmware commands require a Frothy source checkout") ||
		!strings.Contains(err.Error(), "set "+frothySourceRootEnv) {
		t.Fatalf("root = %q, err = %v; want setup guidance", got, err)
	}
	if got != "" {
		t.Fatalf("root = %q, want empty", got)
	}
}

func makeSourceRoot(t *testing.T) string {
	t.Helper()
	root := t.TempDir()
	for _, dir := range []string{"boards", "src"} {
		if err := os.Mkdir(filepath.Join(root, dir), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	for _, path := range []string{"Makefile", filepath.Join("src", "froth.h")} {
		if err := os.WriteFile(filepath.Join(root, path), nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return canonicalPath(root)
}
