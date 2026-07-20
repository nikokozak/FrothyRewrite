package main

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
)

// SPEC D5 + D6 + D9. Resolves a project's deps recursively into a flat
// list ordered dependency-before-dependent so the build's main.fr
// compiles last and library overlay sources compile in an order that
// matches D6's promise. Path deps resolve relative to the parent library;
// git deps resolve through the cache populated by frothy fetch.
//
// Cycle detection is depth-first with a visiting stack. Two deps to
// the same library by the same path are deduped (last wins on a name
// collision).

func resolveDeps(projectDir string, proj projectManifest) ([]resolvedLibrary, error) {
	r := &depResolver{
		projectDir: projectDir,
		seen:       map[string]int{},
		ordered:    nil,
	}
	for _, name := range sortedDepNames(proj.Deps) {
		if err := r.walk(projectDir, name, proj.Deps[name], nil); err != nil {
			return nil, err
		}
	}
	return r.ordered, nil
}

type depResolver struct {
	projectDir string
	// seen maps library name -> index in ordered. Duplicate names by
	// different paths are an error; same name same path is a no-op
	// dedup.
	seen    map[string]int
	ordered []resolvedLibrary
}

func (r *depResolver) walk(parentDir, depName string, dep manifestDep, stack []string) error {
	libPath, isGit, err := depLibraryPath(parentDir, dep)
	if err != nil {
		return fmt.Errorf("dep %s: %w", depName, err)
	}

	for _, s := range stack {
		if s == libPath {
			chain := append(append([]string{}, stack...), libPath)
			chainNames := make([]string, 0, len(chain))
			for _, p := range chain {
				chainNames = append(chainNames, filepath.Base(p))
			}
			return fmt.Errorf("library dependency cycle: %s", joinChain(chainNames))
		}
	}

	if isGit {
		if _, err := os.Stat(libPath); err != nil {
			if os.IsNotExist(err) {
				return fmt.Errorf("dep %s: git dep not fetched; run frothy fetch", depName)
			}
			return fmt.Errorf("dep %s: %w", depName, err)
		}
	}

	var lib loadedLib
	if isGit {
		lib, err = loadGitLibrary(libPath)
	} else {
		lib, err = loadLibrary(libPath)
	}
	if err != nil {
		return fmt.Errorf("dep %s: %w", depName, err)
	}

	if existingIdx, ok := r.seen[lib.resolved.name]; ok {
		if r.ordered[existingIdx].path == lib.resolved.path {
			return nil
		}
		return fmt.Errorf("library %s declared by two different paths: %s and %s", lib.resolved.name, r.ordered[existingIdx].path, lib.resolved.path)
	}

	// Recurse first so dependencies appear earlier in `ordered`.
	if lib.libManifest != nil {
		stack = append(stack, libPath)
		for _, name := range sortedDepNames(lib.libManifest.Deps) {
			if err := r.walk(libPath, name, lib.libManifest.Deps[name], stack); err != nil {
				return err
			}
		}
	}

	r.seen[lib.resolved.name] = len(r.ordered)
	r.ordered = append(r.ordered, lib.resolved)
	return nil
}

func depLibraryPath(parentDir string, dep manifestDep) (string, bool, error) {
	if dep.Git != "" {
		libPath, err := gitDepCacheDir(dep)
		return libPath, true, err
	}
	if dep.Path == "" {
		return "", false, fmt.Errorf("missing path")
	}
	return filepath.Clean(filepath.Join(parentDir, dep.Path)), false, nil
}

// loadedLib carries the resolvedLibrary fields the generator needs
// plus a reference to the parsed lib.toml (for [deps] recursion).
type loadedLib struct {
	libManifest *libraryManifest
	resolved    resolvedLibrary
}

// loadLibrary reads lib.fr (mandatory) and lib.toml (optional). A
// missing lib.toml gives a pure-modules library named after the
// directory.
func loadLibrary(libPath string) (loadedLib, error) {
	return loadLibraryAt(libPath, false, true)
}

func loadGitLibrary(libPath string) (loadedLib, error) {
	return loadLibraryAt(libPath, true, false)
}

func loadLibraryAt(libPath string, requireManifest bool, requireNameMatchesDir bool) (loadedLib, error) {
	info, err := os.Stat(libPath)
	if err != nil {
		return loadedLib{}, fmt.Errorf("library path %s: %w", libPath, err)
	}
	if !info.IsDir() {
		return loadedLib{}, fmt.Errorf("library path %s is not a directory", libPath)
	}
	libFr := filepath.Join(libPath, "lib.fr")
	if _, err := os.Stat(libFr); err != nil {
		return loadedLib{}, fmt.Errorf("library %s: lib.fr missing", filepath.Base(libPath))
	}

	tomlPath := filepath.Join(libPath, "lib.toml")
	tomlBytes, err := os.ReadFile(tomlPath)
	if err != nil {
		if os.IsNotExist(err) {
			if requireManifest {
				return loadedLib{}, fmt.Errorf("git repo missing lib.toml at repo root")
			}
			return loadedLib{
				resolved: resolvedLibrary{
					name: filepath.Base(libPath),
					path: libPath,
				},
			}, nil
		}
		return loadedLib{}, fmt.Errorf("library %s: %w", filepath.Base(libPath), err)
	}
	m, err := parseLibraryManifest(tomlBytes)
	if err != nil {
		return loadedLib{}, err
	}
	if requireNameMatchesDir && m.Name != filepath.Base(libPath) {
		return loadedLib{}, fmt.Errorf("library %s: lib.toml name %q does not match directory", filepath.Base(libPath), m.Name)
	}

	resolved := resolvedLibrary{
		name:     m.Name,
		path:     libPath,
		requires: append([]string{}, m.Requires...),
	}
	if m.Extension != nil {
		resolved.extension = &libraryExtension{
			sources: append([]string{}, m.Extension.Sources...),
		}
	}
	for _, n := range m.Natives {
		resolved.natives = append(resolved.natives, libraryNative{
			name:      n.Name,
			arity:     uint8(n.Arity),
			cFunction: n.CFunction,
		})
	}
	return loadedLib{
		libManifest: &m,
		resolved:    resolved,
	}, nil
}

// boardGateLibraries checks that every dep with a lib.toml lists the
// project's board in its boards array. Pure-modules libraries (no
// lib.toml) implicitly support every board per SPEC D7.
func boardGateLibraries(board string, libs []resolvedLibrary) error {
	for _, lib := range libs {
		manifestPath := filepath.Join(lib.path, "lib.toml")
		bytes, err := os.ReadFile(manifestPath)
		if err != nil {
			if os.IsNotExist(err) {
				continue
			}
			return err
		}
		m, err := parseLibraryManifest(bytes)
		if err != nil {
			return err
		}
		matched := false
		for _, supportedBoard := range m.Boards {
			if supportedBoard == board {
				matched = true
				break
			}
		}
		if !matched {
			return fmt.Errorf("library %s does not support board %s (boards: %s)", lib.name, board, joinChain(m.Boards))
		}
	}
	return nil
}

// capabilityGateLibraries rejects a composition that disables a capability a
// selected library requires. It runs on the fully resolved (flattened) list,
// so transitive library requirements are covered without a second traversal.
// A requirement is satisfied unless the composition explicitly sets it false:
// an absent capability means "profile default" (on for every offered gate),
// not "disabled".
func capabilityGateLibraries(caps map[string]bool, libs []resolvedLibrary) error {
	for _, lib := range libs {
		for _, req := range lib.requires {
			if enabled, present := caps[req]; present && !enabled {
				return fmt.Errorf("library %s requires capability %q, which is disabled in frothy.toml", lib.name, req)
			}
		}
	}
	return nil
}

func sortedDepNames(deps map[string]manifestDep) []string {
	names := make([]string, 0, len(deps))
	for n := range deps {
		names = append(names, n)
	}
	sort.Strings(names)
	return names
}

func joinChain(parts []string) string {
	if len(parts) == 0 {
		return ""
	}
	out := parts[0]
	for _, p := range parts[1:] {
		out += " -> " + p
	}
	return out
}
