package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// makeTempProject builds a directory tree that frothy build can run
// against. Caller adds libs via libsContent.
func makeTempProject(t *testing.T, projectToml, mainFr string, libsContent map[string]string) string {
	t.Helper()
	dir := t.TempDir()
	writeFile(t, filepath.Join(dir, "frothy.toml"), projectToml)
	if mainFr != "" {
		writeFile(t, filepath.Join(dir, "main.fr"), mainFr)
	}
	for relPath, content := range libsContent {
		writeFile(t, filepath.Join(dir, relPath), content)
	}
	return dir
}

func TestRunBuild_PureModulesLibrary(t *testing.T) {
	dir := makeTempProject(t, `name = "blink"
target = "host"

[deps]
servo = { path = "libs/servo" }
`, "servo.attach: 5\n", map[string]string{
		"libs/servo/lib.fr": "to servo.attach with pin [ pin ]\n",
	})
	var stdout, stderr bytes.Buffer
	if err := runBuild(buildOptions{projectDir: dir, skipMake: true}, &stdout, &stderr); err != nil {
		t.Fatalf("runBuild: %v\nstderr: %s", err, stderr.String())
	}
	// generator outputs present
	out := filepath.Join(dir, ".frothy", "build", "host")
	for _, f := range []string{"libs.cmake", "lib_natives.c", "program.fr"} {
		if _, err := os.Stat(filepath.Join(out, f)); err != nil {
			t.Errorf("missing output %s: %v", f, err)
		}
	}
	// pure-modules library should leave lib_natives.c with the no-natives marker
	content, _ := os.ReadFile(filepath.Join(out, "lib_natives.c"))
	if !strings.Contains(string(content), "No library natives.") {
		t.Errorf("expected no-natives marker in lib_natives.c; got: %s", content)
	}
	// program.fr should contain both library and main source
	program, _ := os.ReadFile(filepath.Join(out, "program.fr"))
	if !strings.Contains(string(program), "servo.attach") {
		t.Errorf("program.fr missing library word")
	}
	if !strings.Contains(string(program), "library: servo") {
		t.Errorf("program.fr missing library marker")
	}
	if !strings.Contains(string(program), "main") {
		t.Errorf("program.fr missing main marker")
	}
}

func TestRunBuild_MixedLibraryEmitsNatives(t *testing.T) {
	dir := makeTempProject(t, `name = "stage"
target = "host"

[deps]
neopixel = { path = "libs/neopixel" }
`, "neopixel.show:\n", map[string]string{
		"libs/neopixel/lib.fr":            "to neopixel.use [ ]\n",
		"libs/neopixel/native/neopixel.c": "/* extension */\n",
		"libs/neopixel/lib.toml": `name = "neopixel"
targets = ["host"]

[extension]
sources = ["native/neopixel.c"]

[[natives]]
name = "neopixel.show"
arity = 1
c_function = "fr_lib_neopixel_show"
`,
	})
	var stdout, stderr bytes.Buffer
	if err := runBuild(buildOptions{projectDir: dir, skipMake: true}, &stdout, &stderr); err != nil {
		t.Fatalf("runBuild: %v\nstderr: %s", err, stderr.String())
	}
	out := filepath.Join(dir, ".frothy", "build", "host")
	cmake, _ := os.ReadFile(filepath.Join(out, "libs.cmake"))
	if !strings.Contains(string(cmake), "neopixel/native/neopixel.c") {
		t.Errorf("libs.cmake missing extension source: %s", cmake)
	}
	natives, _ := os.ReadFile(filepath.Join(out, "lib_natives.c"))
	for _, want := range []string{
		`extern fr_err_t fr_lib_neopixel_show(`,
		`"neopixel.show", fr_lib_neopixel_show, 1`,
		`fr_lib_natives_count = 1`,
	} {
		if !strings.Contains(string(natives), want) {
			t.Errorf("lib_natives.c missing %q", want)
		}
	}
}

func TestRunBuild_TargetGateFailure(t *testing.T) {
	dir := makeTempProject(t, `name = "blink"
target = "atmega328p"

[deps]
neopixel = { path = "libs/neopixel" }
`, "", map[string]string{
		"libs/neopixel/lib.fr":            "",
		"libs/neopixel/native/neopixel.c": "",
		"libs/neopixel/lib.toml": `name = "neopixel"
targets = ["esp32_devkit_v1"]

[extension]
sources = ["native/neopixel.c"]
`,
	})
	var stdout, stderr bytes.Buffer
	err := runBuild(buildOptions{projectDir: dir, skipMake: true}, &stdout, &stderr)
	if err == nil {
		t.Fatal("expected target gate to fail")
	}
	want := "library neopixel does not support target atmega328p"
	if !strings.Contains(err.Error(), want) {
		t.Fatalf("got %q, want it to contain %q", err.Error(), want)
	}
}

func TestRunBuild_LibraryToLibraryDep(t *testing.T) {
	dir := makeTempProject(t, `name = "show"
target = "host"

[deps]
stage = { path = "libs/stage" }
`, "", map[string]string{
		"libs/servo/lib.fr": "to servo.attach [ ]\n",
		"libs/stage/lib.fr": "to stage.go [ ]\n",
		"libs/stage/lib.toml": `name = "stage"
targets = ["host"]

[deps]
servo = { path = "../servo" }
`,
	})
	var stdout, stderr bytes.Buffer
	if err := runBuild(buildOptions{projectDir: dir, skipMake: true}, &stdout, &stderr); err != nil {
		t.Fatalf("runBuild: %v\nstderr: %s", err, stderr.String())
	}
	// program.fr should have servo before stage (D6: dependency before
	// dependent).
	program, _ := os.ReadFile(filepath.Join(dir, ".frothy", "build", "host", "program.fr"))
	svIdx := strings.Index(string(program), "library: servo")
	stIdx := strings.Index(string(program), "library: stage")
	if svIdx < 0 || stIdx < 0 {
		t.Fatalf("missing library markers in program.fr")
	}
	if svIdx >= stIdx {
		t.Fatalf("servo should appear before stage; got svIdx=%d stIdx=%d", svIdx, stIdx)
	}
}

func TestRunBuild_IncludeResolved(t *testing.T) {
	dir := makeTempProject(t, `name = "blink"
target = "host"

[deps]
math = { path = "libs/math" }
`, "", map[string]string{
		"libs/math/lib.fr":     "include \"helpers.fr\"\nto math.use [ math.double: 21 ]\n",
		"libs/math/helpers.fr": "to math.double with n [ n n + ]\n",
	})
	var stdout, stderr bytes.Buffer
	if err := runBuild(buildOptions{projectDir: dir, skipMake: true}, &stdout, &stderr); err != nil {
		t.Fatalf("runBuild: %v\nstderr: %s", err, stderr.String())
	}
	program, _ := os.ReadFile(filepath.Join(dir, ".frothy", "build", "host", "program.fr"))
	if !strings.Contains(string(program), "math.double with n") {
		t.Errorf("include not preprocessed; program.fr: %s", program)
	}
}

func TestRunBuildCommand_MissingManifest(t *testing.T) {
	dir := t.TempDir()
	var stdout, stderr bytes.Buffer
	rc := runBuildCommand([]string{"--project", dir, "--no-make"}, &stdout, &stderr)
	if rc == 0 {
		t.Fatal("expected nonzero exit code on missing frothy.toml")
	}
	if !strings.Contains(stderr.String(), "frothy.toml") {
		t.Fatalf("expected stderr to mention frothy.toml; got: %s", stderr.String())
	}
}
