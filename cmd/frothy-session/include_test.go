package main

import (
	"fmt"
	"strings"
	"testing"
)

func mapLoad(files map[string]string) func(string) (string, error) {
	return func(path string) (string, error) {
		s, ok := files[path]
		if !ok {
			return "", fmt.Errorf("file not found: %s", path)
		}
		return s, nil
	}
}

func TestPreprocessIncludeNoDirectives(t *testing.T) {
	files := map[string]string{
		"proj/main.fr": "to foo [ 1 ]\nto bar [ 2 ]\n",
	}
	got, err := preprocessInclude("proj/main.fr", mapLoad(files))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	want := "to foo [ 1 ]\nto bar [ 2 ]\n"
	if got != want {
		t.Fatalf("mismatch:\nwant: %q\ngot:  %q", want, got)
	}
}

func TestPreprocessIncludeSplicesSibling(t *testing.T) {
	files := map[string]string{
		"libs/scale/lib.fr":   "include \"limit.fr\"\nto scale.percent [ ]\n",
		"libs/scale/limit.fr": "to scale.bounded [ ]\n",
	}
	got, err := preprocessInclude("libs/scale/lib.fr", mapLoad(files))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	want := "to scale.bounded [ ]\nto scale.percent [ ]\n"
	if got != want {
		t.Fatalf("mismatch:\nwant: %q\ngot:  %q", want, got)
	}
}

func TestPreprocessIncludeNested(t *testing.T) {
	files := map[string]string{
		"libs/x/lib.fr": "include \"a.fr\"\nto x.tail [ ]\n",
		"libs/x/a.fr":   "include \"b.fr\"\nto x.a [ ]\n",
		"libs/x/b.fr":   "to x.b [ ]\n",
	}
	got, err := preprocessInclude("libs/x/lib.fr", mapLoad(files))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	want := "to x.b [ ]\nto x.a [ ]\nto x.tail [ ]\n"
	if got != want {
		t.Fatalf("mismatch:\nwant: %q\ngot:  %q", want, got)
	}
}

func TestPreprocessIncludeMultipleSiblings(t *testing.T) {
	files := map[string]string{
		"proj/main.fr": "include \"a.fr\"\ninclude \"b.fr\"\nto main [ ]\n",
		"proj/a.fr":    "to a [ ]\n",
		"proj/b.fr":    "to b [ ]\n",
	}
	got, err := preprocessInclude("proj/main.fr", mapLoad(files))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	want := "to a [ ]\nto b [ ]\nto main [ ]\n"
	if got != want {
		t.Fatalf("mismatch:\nwant: %q\ngot:  %q", want, got)
	}
}

func TestPreprocessIncludeDirectCycle(t *testing.T) {
	files := map[string]string{
		"libs/x/lib.fr": "include \"lib.fr\"\n",
	}
	_, err := preprocessInclude("libs/x/lib.fr", mapLoad(files))
	if err == nil {
		t.Fatalf("expected cycle error, got nil")
	}
	want := "include cycle: libs/x/lib.fr -> libs/x/lib.fr"
	if err.Error() != want {
		t.Fatalf("mismatch:\nwant: %q\ngot:  %q", want, err.Error())
	}
}

func TestPreprocessIncludeIndirectCycle(t *testing.T) {
	files := map[string]string{
		"libs/servo/lib.fr":  "include \"math.fr\"\n",
		"libs/servo/math.fr": "include \"lib.fr\"\n",
	}
	_, err := preprocessInclude("libs/servo/lib.fr", mapLoad(files))
	if err == nil {
		t.Fatalf("expected cycle error, got nil")
	}
	want := "include cycle: libs/servo/lib.fr -> libs/servo/math.fr -> libs/servo/lib.fr"
	if err.Error() != want {
		t.Fatalf("mismatch:\nwant: %q\ngot:  %q", want, err.Error())
	}
}

func TestPreprocessIncludeRootNotFound(t *testing.T) {
	_, err := preprocessInclude("proj/missing.fr", mapLoad(nil))
	if err == nil {
		t.Fatalf("expected load error, got nil")
	}
	if !strings.Contains(err.Error(), "file not found") {
		t.Fatalf("expected load error to surface, got: %v", err)
	}
}

func TestPreprocessIncludeTargetNotFound(t *testing.T) {
	files := map[string]string{
		"proj/main.fr": "include \"missing.fr\"\n",
	}
	_, err := preprocessInclude("proj/main.fr", mapLoad(files))
	if err == nil {
		t.Fatalf("expected load error, got nil")
	}
	if !strings.Contains(err.Error(), "file not found") {
		t.Fatalf("expected load error to surface, got: %v", err)
	}
}

func TestPreprocessIncludeIncludedFileMissingTrailingNewline(t *testing.T) {
	files := map[string]string{
		"proj/main.fr": "include \"a.fr\"\nto main [ ]\n",
		"proj/a.fr":    "to a [ ]",
	}
	got, err := preprocessInclude("proj/main.fr", mapLoad(files))
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	want := "to a [ ]\nto main [ ]\n"
	if got != want {
		t.Fatalf("mismatch:\nwant: %q\ngot:  %q", want, got)
	}
}

func TestMatchInclude(t *testing.T) {
	cases := []struct {
		in   string
		want string
		ok   bool
	}{
		{"include \"helpers.fr\"\n", "helpers.fr", true},
		{"include \"a.fr\"", "a.fr", true},
		{"  include \"a.fr\"\n", "a.fr", true},
		{"\tinclude \"a.fr\"\n", "a.fr", true},
		{"include  \"a.fr\"\n", "a.fr", true},
		{"include \"a.fr\"  \n", "a.fr", true},

		{"\n", "", false},
		{"to include with X [ ]\n", "", false},
		{"include\n", "", false},
		{"include \n", "", false},
		{"include foo\n", "", false},
		{"include\"a.fr\"\n", "", false},
		{"include \"\"\n", "", false},
		{"include \"a.fr\" extra\n", "", false},
		{"include \"a.fr\n", "", false},
		{"iinclude \"a.fr\"\n", "", false},
		{"included \"a.fr\"\n", "", false},
	}
	for _, c := range cases {
		got, ok := matchInclude(c.in)
		if ok != c.ok || got != c.want {
			t.Errorf("matchInclude(%q) = (%q, %v); want (%q, %v)",
				c.in, got, ok, c.want, c.ok)
		}
	}
}
