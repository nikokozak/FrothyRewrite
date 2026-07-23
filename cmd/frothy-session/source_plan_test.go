package main

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSourcePlanResolvesIncludesInPlace(t *testing.T) {
	root := t.TempDir()
	writeSourcePlanFile(t, root, "frothy.toml", "name = \"web\"\nboard = \"esp32_devkit_v1\"\n")
	writeSourcePlanFile(t, root, "main.fr", "include \"src/led.fr\"\nmain is fn [ led: ]\n")
	writeSourcePlanFile(t, root, "src/led.fr", "include \"pin.fr\"\nled is fn [ pin ]\n")
	writeSourcePlanFile(t, root, "src/pin.fr", "pin is 13\n")

	var stdout, stderr bytes.Buffer
	if code := runSourcePlanCommand([]string{"--project", root}, &stdout, &stderr); code != 0 {
		t.Fatalf("source-plan exit = %d, stderr = %q, stdout = %q", code, stderr.String(), stdout.String())
	}
	var result sourcePlanResult
	if err := json.Unmarshal(stdout.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if result.Kind != "resolved" || result.ResolverVersion != 1 || result.Board != "esp32_devkit_v1" {
		t.Fatalf("unexpected result: %#v", result)
	}
	want := "pin is 13\nled is fn [ pin ]\nmain is fn [ led: ]\n"
	if len(result.Sources) != 1 || result.Sources[0].Path != "main.fr" || result.Sources[0].Source != want {
		t.Fatalf("sources = %#v, want main.fr %q", result.Sources, want)
	}
}

func TestSourcePlanResolvesAnEntryOtherThanMain(t *testing.T) {
	root := t.TempDir()
	writeSourcePlanFile(t, root, "frothy.toml", "name = \"web\"\nboard = \"esp32_devkit_v1\"\n")
	writeSourcePlanFile(t, root, "main.fr", "main is fn [ 1 ]\n")
	writeSourcePlanFile(t, root, "src/led.fr", "include \"pin.fr\"\nled is fn [ pin ]\n")
	writeSourcePlanFile(t, root, "src/pin.fr", "pin is 13\n")

	var stdout, stderr bytes.Buffer
	args := []string{"--project", root, "--entry", "src/led.fr"}
	if code := runSourcePlanCommand(args, &stdout, &stderr); code != 0 {
		t.Fatalf("source-plan exit = %d, stderr = %q, stdout = %q", code, stderr.String(), stdout.String())
	}
	var result sourcePlanResult
	if err := json.Unmarshal(stdout.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	want := "pin is 13\nled is fn [ pin ]\n"
	if result.Kind != "resolved" || len(result.Sources) != 1 ||
		result.Sources[0].Path != "src/led.fr" || result.Sources[0].Source != want {
		t.Fatalf("sources = %#v, want src/led.fr %q", result.Sources, want)
	}
}

func TestSourcePlanRejectsAnEntryOutsideTheProject(t *testing.T) {
	root := t.TempDir()
	writeSourcePlanFile(t, root, "frothy.toml", "name = \"web\"\nboard = \"esp32_devkit_v1\"\n")
	writeSourcePlanFile(t, root, "main.fr", "main is fn [ 1 ]\n")

	var stdout bytes.Buffer
	args := []string{"--project", root, "--entry", "../outside.fr"}
	if code := runSourcePlanCommand(args, &stdout, &bytes.Buffer{}); code != 1 {
		t.Fatalf("source-plan exit = %d, stdout = %q", code, stdout.String())
	}
	var result sourcePlanResult
	if err := json.Unmarshal(stdout.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if result.Kind != "error" || !strings.Contains(result.Message, "escapes the project") {
		t.Fatalf("unexpected result: %#v", result)
	}
}

func TestSourcePlanConfinesEveryInclude(t *testing.T) {
	root := t.TempDir()
	writeSourcePlanFile(t, root, "frothy.toml", "name = \"web\"\nboard = \"esp32_devkit_v1\"\n")
	outside := filepath.Join(t.TempDir(), "secret.fr")
	if err := os.WriteFile(outside, []byte("secret is 1\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	tests := []struct {
		name    string
		main    string
		prepare func()
		want    string
	}{
		{name: "parent", main: "include \"../secret.fr\"\n", want: "escapes the project"},
		{name: "absolute", main: "include \"/etc/passwd\"\n", want: "absolute include path"},
		{name: "missing", main: "include \"missing.fr\"\n", want: "does not exist"},
		{name: "cycle", main: "include \"main.fr\"\n", want: "include cycle"},
		{
			name: "symlink",
			main: "include \"linked.fr\"\n",
			prepare: func() {
				if err := os.Symlink(outside, filepath.Join(root, "linked.fr")); err != nil {
					t.Skipf("symlink unavailable: %v", err)
				}
			},
			want: "escapes the project",
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if test.prepare != nil {
				test.prepare()
			}
			writeSourcePlanFile(t, root, "main.fr", test.main)
			var stdout bytes.Buffer
			if code := runSourcePlanCommand([]string{"--project", root}, &stdout, &bytes.Buffer{}); code != 1 {
				t.Fatalf("source-plan exit = %d, want 1", code)
			}
			var result sourcePlanResult
			if err := json.Unmarshal(stdout.Bytes(), &result); err != nil {
				t.Fatal(err)
			}
			if result.Kind != "error" || !strings.Contains(result.Message, test.want) {
				t.Fatalf("result = %#v, want error containing %q", result, test.want)
			}
		})
	}
}

func TestSourcePlanBoundsRepeatedIncludes(t *testing.T) {
	root := t.TempDir()
	writeSourcePlanFile(t, root, "frothy.toml", "name = \"web\"\nboard = \"esp32_devkit_v1\"\n")
	writeSourcePlanFile(t, root, "main.fr", strings.Repeat("include \"large.fr\"\n", 5))
	writeSourcePlanFile(t, root, "large.fr", strings.Repeat("x", 64*1024))

	var stdout bytes.Buffer
	if code := runSourcePlanCommand([]string{"--project", root}, &stdout, &bytes.Buffer{}); code != 1 {
		t.Fatalf("source-plan exit = %d, want 1", code)
	}
	var result sourcePlanResult
	if err := json.Unmarshal(stdout.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(result.Message, "exceeds 262144 source bytes") {
		t.Fatalf("result = %#v", result)
	}
}

func TestSourcePlanRejectsOneOversizedFile(t *testing.T) {
	root := t.TempDir()
	writeSourcePlanFile(t, root, "frothy.toml", "name = \"web\"\nboard = \"esp32_devkit_v1\"\n")
	writeSourcePlanFile(t, root, "main.fr", strings.Repeat("x", sourcePlanByteLimit+1))

	var stdout bytes.Buffer
	if code := runSourcePlanCommand([]string{"--project", root}, &stdout, &bytes.Buffer{}); code != 1 {
		t.Fatalf("source-plan exit = %d, want 1", code)
	}
	var result sourcePlanResult
	if err := json.Unmarshal(stdout.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(result.Message, "exceeds 262144 source bytes") {
		t.Fatalf("result = %#v", result)
	}
}

func TestSourcePlanReportsPinnedDependenciesWithoutFetching(t *testing.T) {
	root := t.TempDir()
	writeSourcePlanFile(t, root, "frothy.toml", `name = "web"
board = "esp32_devkit_v1"
[deps]
sensor = { git = "https://github.com/example/sensor.git", rev = "0123456789abcdef" }
`)
	writeSourcePlanFile(t, root, "main.fr", "main is 1\n")

	var stdout bytes.Buffer
	if code := runSourcePlanCommand([]string{"--project", root}, &stdout, &bytes.Buffer{}); code != 0 {
		t.Fatalf("source-plan exit = %d, stdout = %q", code, stdout.String())
	}
	var result sourcePlanResult
	if err := json.Unmarshal(stdout.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if result.Kind != "needs_dependencies" || len(result.Dependencies) != 1 {
		t.Fatalf("result = %#v", result)
	}
	dependency := result.Dependencies[0]
	if dependency.Name != "sensor" || dependency.Kind != "git" || dependency.Rev != "0123456789abcdef" {
		t.Fatalf("dependency = %#v", dependency)
	}
}

func writeSourcePlanFile(t *testing.T, root, path, source string) {
	t.Helper()
	fullPath := filepath.Join(root, path)
	if err := os.MkdirAll(filepath.Dir(fullPath), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(fullPath, []byte(source), 0o644); err != nil {
		t.Fatal(err)
	}
}
