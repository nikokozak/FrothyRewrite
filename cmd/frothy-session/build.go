package main

import (
	"bytes"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
)

// frothy build reads frothy.toml, resolves deps, board-gates, emits
// libs.cmake + lib_natives.c into .frothy/build/<board>/, writes the
// merged lib.fr stream to library.fr (consumed by `frothy install`)
// and the include-resolved main.fr to main.fr (consumed by `frothy
// send`), then invokes make for the kernel build. Flashing is a
// separate verb (`flash`); this one only builds.

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
	if helpRequested(args) {
		printVerbHelp(stdout, helpFor("build"), fs)
		return 0
	}
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
	if err := fetchDeps(opts.projectDir, proj); err != nil {
		return err
	}
	libs, err := resolveDeps(opts.projectDir, proj)
	if err != nil {
		return err
	}
	if err := boardGateLibraries(proj.Board, libs); err != nil {
		return err
	}
	if err := capabilityGateLibraries(proj.Capabilities, libs); err != nil {
		return err
	}
	if err := emitCompositionFiles(opts.projectDir, proj.Board, proj.Capabilities); err != nil {
		return err
	}
	if err := emitGeneratedFiles(opts.projectDir, proj.Board, libs); err != nil {
		return err
	}
	if err := emitLibrarySource(opts.projectDir, proj.Board, libs); err != nil {
		return err
	}
	if err := emitMainSource(opts.projectDir, proj.Board); err != nil {
		return err
	}
	if opts.skipMake {
		fmt.Fprintf(stdout, "frothy build: generator emission complete for board %s (--no-make set)\n", proj.Board)
		return nil
	}
	return runMake(opts.projectDir, proj.Board, stdout, stderr)
}

func readProjectManifest(projectDir string) (projectManifest, error) {
	bytes, err := os.ReadFile(filepath.Join(projectDir, "frothy.toml"))
	if err != nil {
		return projectManifest{}, fmt.Errorf("read frothy.toml: %w", err)
	}
	return parseProjectManifest(bytes)
}

// .frothy/build/<board>/ is the SPEC's neutral output path (D10 +
// Resolved-during-pre-loop). Survives changes to CMake/make build dirs.
func buildOutputDir(projectDir, board string) string {
	return filepath.Join(projectDir, ".frothy", "build", board)
}

// emitCompositionFiles writes composition.h and composition.sdkconfig only
// when the manifest deviates from the profile's default capabilities, and
// only when their content changes — both are ESP-IDF configure inputs, so a
// no-op rewrite would churn the incremental build. A default build removes any
// stale files so make sees profile defaults again.
func emitCompositionFiles(projectDir, board string, caps map[string]bool) error {
	disabled := disabledCapabilities(caps)
	outDir := buildOutputDir(projectDir, board)
	headerPath := filepath.Join(outDir, "composition.h")
	sdkPath := filepath.Join(outDir, "composition.sdkconfig")
	if len(disabled) == 0 {
		for _, p := range []string{headerPath, sdkPath} {
			if err := os.Remove(p); err != nil && !os.IsNotExist(err) {
				return err
			}
		}
		return nil
	}
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return err
	}
	if err := writeFileIfChanged(headerPath, []byte(generatedCompositionH(disabled))); err != nil {
		return err
	}
	return writeFileIfChanged(sdkPath, []byte(generatedCompositionSdkconfig(disabled)))
}

func writeFileIfChanged(path string, content []byte) error {
	if existing, err := os.ReadFile(path); err == nil && bytes.Equal(existing, content) {
		return nil
	}
	return os.WriteFile(path, content, 0o644)
}

func emitGeneratedFiles(projectDir, board string, libs []resolvedLibrary) error {
	outDir := buildOutputDir(projectDir, board)
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

// Sections are separated by a single newline (between lib.fr's and
// between libraries and main.fr). Frothy has no comment token, so any
// `# ---` header would fail to parse; the kernel compiler tolerates
// blank lines and `see <word>` finds source by name regardless.
func sourceLoader(path string) (string, error) {
	bytes, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	return string(bytes), nil
}

func emitLibrarySource(projectDir, board string, libs []resolvedLibrary) error {
	var merged []byte
	for _, lib := range libs {
		src, err := preprocessInclude(filepath.Join(lib.path, "lib.fr"), sourceLoader)
		if err != nil {
			return fmt.Errorf("library %s: %w", lib.name, err)
		}
		if len(merged) > 0 {
			merged = append(merged, '\n')
		}
		merged = append(merged, []byte(src)...)
	}
	outPath := filepath.Join(buildOutputDir(projectDir, board), "library.fr")
	return os.WriteFile(outPath, merged, 0o644)
}

func emitMainSource(projectDir, board string) error {
	mainFr := filepath.Join(projectDir, "main.fr")
	var content []byte
	if _, err := os.Stat(mainFr); err == nil {
		src, err := preprocessInclude(mainFr, sourceLoader)
		if err != nil {
			return fmt.Errorf("main.fr: %w", err)
		}
		content = []byte(src)
	}
	outDir := buildOutputDir(projectDir, board)
	outPath := filepath.Join(outDir, "main.fr")
	if err := os.WriteFile(outPath, content, 0o644); err != nil {
		return err
	}
	// D9 retired program.fr; drop any copy left behind by a pre-split build
	// so `frothy install` and `frothy send` never see the obsolete artifact.
	if err := os.Remove(filepath.Join(outDir, "program.fr")); err != nil && !os.IsNotExist(err) {
		return err
	}
	return nil
}

func runMake(projectDir, board string, stdout io.Writer, stderr io.Writer) error {
	sourceRoot, err := resolveFrothySourceRoot(projectDir)
	if err != nil {
		return err
	}
	outDir := buildOutputDir(projectDir, board)
	args := []string{"-C", sourceRoot, "artifacts", "BOARD=" + board,
		"FROTHY_LIB_NATIVES_C=" + filepath.Join(outDir, "lib_natives.c"),
		"FROTHY_LIBS_CMAKE=" + filepath.Join(outDir, "libs.cmake"),
	}
	// Pass the composition overrides only when emitCompositionFiles wrote them;
	// their absence means profile defaults, which the Makefile treats as unset.
	compositionH := filepath.Join(outDir, "composition.h")
	if _, err := os.Stat(compositionH); err == nil {
		args = append(args, "FROTHY_COMPOSITION_H="+compositionH)
	}
	compositionSdkconfig := filepath.Join(outDir, "composition.sdkconfig")
	if _, err := os.Stat(compositionSdkconfig); err == nil {
		args = append(args, "FROTHY_COMPOSITION_SDKCONFIG="+compositionSdkconfig)
	}
	cmd := exec.Command("make", args...)
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	return cmd.Run()
}
