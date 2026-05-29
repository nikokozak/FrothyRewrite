package main

import (
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"testing"
)

func TestAppendHistoryDedupAndCap(t *testing.T) {
	cases := []struct {
		name string
		in   []string
		add  []string
		want []string
	}{
		{"skip empty", nil, []string{""}, nil},
		{"keep distinct", nil, []string{"a", "b"}, []string{"a", "b"}},
		{"dedup adjacent", nil, []string{"a", "a"}, []string{"a"}},
		{"non-adjacent allowed", nil, []string{"a", "b", "a"}, []string{"a", "b", "a"}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := tc.in
			for _, l := range tc.add {
				got = appendHistory(got, l)
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Fatalf("got %q want %q", got, tc.want)
			}
		})
	}

	t.Run("cap at historyMax", func(t *testing.T) {
		var got []string
		for i := 0; i < historyMax+10; i++ {
			got = appendHistory(got, line(i))
		}
		if len(got) != historyMax {
			t.Fatalf("len = %d, want %d", len(got), historyMax)
		}
		if got[0] != line(10) || got[len(got)-1] != line(historyMax+9) {
			t.Fatalf("oldest=%q newest=%q", got[0], got[len(got)-1])
		}
	})
}

func TestHistoryRoundTrip(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "history")
	in := []string{"foo", "bar", "baz"}
	if err := saveHistory(path, in); err != nil {
		t.Fatal(err)
	}
	out, err := loadHistory(path)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(out, in) {
		t.Fatalf("got %q want %q", out, in)
	}
}

func TestLoadHistoryMissingFile(t *testing.T) {
	out, err := loadHistory(filepath.Join(t.TempDir(), "nope"))
	if err != nil {
		t.Fatal(err)
	}
	if out != nil {
		t.Fatalf("got %q want nil", out)
	}
}

func TestLoadHistoryFiltersAdjacentDuplicatesAndBlanks(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "history")
	raw := "foo\nfoo\n\nbar\nbar\nfoo\n"
	if err := saveHistory(path, strings.Split(strings.TrimRight(raw, "\n"), "\n")); err != nil {
		t.Fatal(err)
	}
	out, err := loadHistory(path)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"foo", "bar", "foo"}
	if !reflect.DeepEqual(out, want) {
		t.Fatalf("got %q want %q", out, want)
	}
}

func TestDefaultHistoryPath(t *testing.T) {
	cases := []struct {
		name string
		env  map[string]string
		want string
	}{
		{"XDG wins", map[string]string{"XDG_DATA_HOME": "/x", "HOME": "/h"}, "/x/frothy/history"},
		{"HOME fallback", map[string]string{"HOME": "/h"}, "/h/.local/share/frothy/history"},
		{"neither", map[string]string{}, ""},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := defaultHistoryPath(envMap(tc.env))
			if got != tc.want {
				t.Fatalf("got %q want %q", got, tc.want)
			}
		})
	}
}

func TestResolveHistoryConfig(t *testing.T) {
	dir := t.TempDir()
	existing := filepath.Join(dir, "h")
	if err := saveHistory(existing, []string{"alpha", "beta"}); err != nil {
		t.Fatal(err)
	}

	t.Run("no-history disables", func(t *testing.T) {
		got := resolveHistoryConfig(true, existing, envMap(nil))
		if got.enabled || got.path != "" || got.initial != nil {
			t.Fatalf("got %+v", got)
		}
	})
	t.Run("override path loads initial", func(t *testing.T) {
		got := resolveHistoryConfig(false, existing, envMap(nil))
		if !got.enabled || got.path != existing {
			t.Fatalf("got %+v", got)
		}
		if !reflect.DeepEqual(got.initial, []string{"alpha", "beta"}) {
			t.Fatalf("initial = %q", got.initial)
		}
	})
	t.Run("default uses XDG", func(t *testing.T) {
		got := resolveHistoryConfig(false, "", envMap(map[string]string{"XDG_DATA_HOME": "/tmp/x"}))
		if got.path != "/tmp/x/frothy/history" {
			t.Fatalf("path = %q", got.path)
		}
	})
	t.Run("no env, no override → disabled", func(t *testing.T) {
		got := resolveHistoryConfig(false, "", envMap(nil))
		if got.enabled {
			t.Fatalf("got %+v", got)
		}
	})
}

func envMap(m map[string]string) func(string) string {
	return func(k string) string { return m[k] }
}

func line(i int) string {
	return "line-" + strconv.Itoa(i)
}
