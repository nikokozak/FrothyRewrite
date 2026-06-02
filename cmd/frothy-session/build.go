package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
)

// SPEC D10: frothy build reads frothy.toml, resolves deps, target-
// gates, emits libs.cmake + lib_natives.c into .frothy/build/<target>/,
// merges all lib.fr's + main.fr into program.fr (include-resolved),
// and invokes make for the kernel build. Flashing is a separate verb
// (`flash`); this one only builds.
//
// Two main paths in the build verb:
//   - planBuild: reads + resolves + generates + writes files. Pure-ish
//     (does disk I/O but no network, no exec). Tested directly.
//   - runMake: invokes `make BOARD=<target>`. Skipped in unit tests
//     because it builds the actual kernel.

type buildOptions struct {
	projectDir string
	skipMake   bool // for tests
}

func runBuildMain() int {
	return runBuildCommand(os.Args[1:], os.Stdout, os.Stderr)
}

func runBuildCommand(args []string, stdout io.Writer, stderr io.Writer) int {
	fs := flag.NewFlagSet("frothy build", flag.ContinueOnError)
	fs.SetOutput(stderr)
	projectDir := fs.String("project", ".", "project directory containing frothy.toml")
	skipMake := fs.Bool("no-make", false, "stop after generator emission; do not invoke make")
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if fs.NArg() != 0 {
		fmt.Fprintln(stderr, "frothy build: unexpected positional argument(s)")
		return 2
	}
	absProject, err := filepath.Abs(*projectDir)
	if err != nil {
		fmt.Fprintf(stderr, "frothy build: %v\n", err)
		return 1
	}
	opts := buildOptions{projectDir: absProject, skipMake: *skipMake}
	if err := runBuild(opts, stdout, stderr); err != nil {
		fmt.Fprintf(stderr, "frothy build: %v\n", err)
		return 1
	}
	return 0
}

func runBuild(opts buildOptions, stdout io.Writer, stderr io.Writer) error {
	proj, err := readProjectManifest(opts.projectDir)
	if err != nil {
		return err
	}
	libs, err := resolveDeps(opts.projectDir, proj)
	if err != nil {
		return err
	}
	if err := targetGateLibraries(proj.Target, libs, opts.projectDir); err != nil {
		return err
	}
	if err := emitGeneratedFiles(opts.projectDir, proj.Target, libs); err != nil {
		return err
	}
	if err := emitOverlaySource(opts.projectDir, proj.Target, libs); err != nil {
		return err
	}
	if opts.skipMake {
		fmt.Fprintf(stdout, "frothy build: generator emission complete for target %s (--no-make set)\n", proj.Target)
		return nil
	}
	return runMake(opts.projectDir, proj.Target, stdout, stderr)
}

func readProjectManifest(projectDir string) (projectManifest, error) {
	bytes, err := os.ReadFile(filepath.Join(projectDir, "frothy.toml"))
	if err != nil {
		return projectManifest{}, fmt.Errorf("read frothy.toml: %w", err)
	}
	return parseProjectManifest(bytes)
}

// .frothy/build/<target>/ is the SPEC's neutral output path (D10 +
// Resolved-during-pre-loop). Survives changes to CMake/make build dirs.
func buildOutputDir(projectDir, target string) string {
	return filepath.Join(projectDir, ".frothy", "build", target)
}

func emitGeneratedFiles(projectDir, target string, libs []resolvedLibrary) error {
	outDir := buildOutputDir(projectDir, target)
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return err
	}
	cmake := generatedLibsCMake(libs)
	if err := os.WriteFile(filepath.Join(outDir, "libs.cmake"), []byte(cmake), 0o644); err != nil {
		return err
	}
	natives, err := generatedLibNativesC(libs)
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(outDir, "lib_natives.c"), []byte(natives), 0o644); err != nil {
		return err
	}
	return nil
}

// emitOverlaySource concatenates every library's lib.fr (in dep order)
// then the project's main.fr, with includes preprocessed throughout.
// The result lives at .frothy/build/<target>/program.fr and is what
// frothy-compile-overlay turns into bytecode in a future step.
func emitOverlaySource(projectDir, target string, libs []resolvedLibrary) error {
	loader := func(path string) (string, error) {
		bytes, err := os.ReadFile(path)
		if err != nil {
			return "", err
		}
		return string(bytes), nil
	}
	var merged []byte
	for _, lib := range libs {
		src, err := preprocessInclude(filepath.Join(lib.path, "lib.fr"), loader)
		if err != nil {
			return fmt.Errorf("library %s: %w", lib.name, err)
		}
		merged = append(merged, []byte("\n# --- library: "+lib.name+" ---\n")...)
		merged = append(merged, []byte(src)...)
	}
	mainFr := filepath.Join(projectDir, "main.fr")
	if _, err := os.Stat(mainFr); err == nil {
		src, err := preprocessInclude(mainFr, loader)
		if err != nil {
			return fmt.Errorf("main.fr: %w", err)
		}
		merged = append(merged, []byte("\n# --- main ---\n")...)
		merged = append(merged, []byte(src)...)
	}
	outPath := filepath.Join(buildOutputDir(projectDir, target), "program.fr")
	return os.WriteFile(outPath, merged, 0o644)
}

func runMake(projectDir, target string, stdout io.Writer, stderr io.Writer) error {
	repoRoot := findRepoRoot(projectDir)
	if repoRoot == "" {
		return fmt.Errorf("frothy build: cannot locate repo root (no Makefile up the tree from %s)", projectDir)
	}
	args := []string{"-C", repoRoot, "artifacts", "BOARD=" + target,
		"FROTHY_LIB_NATIVES_C=" + filepath.Join(buildOutputDir(projectDir, target), "lib_natives.c"),
		"FROTHY_LIBS_CMAKE=" + filepath.Join(buildOutputDir(projectDir, target), "libs.cmake"),
	}
	cmd := exec.Command("make", args...)
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	return cmd.Run()
}

// findRepoRoot walks upward from start looking for a Makefile. The
// build verb may run from a sub-project directory; the kernel
// Makefile lives at the FrothyRewrite repo root.
func findRepoRoot(start string) string {
	dir, err := filepath.Abs(start)
	if err != nil {
		return ""
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "Makefile")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return ""
		}
		dir = parent
	}
}
