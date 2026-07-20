package main

import (
	"fmt"

	"github.com/BurntSushi/toml"
)

// SPEC D5 + D7 + D13. frothy.toml at the project root declares name,
// board, and deps. lib.toml inside a library declares name, version,
// boards, extension sources, and natives. parseProjectManifest and
// parseLibraryManifest take the raw bytes and return validated structs.
// Disk I/O sits in the build verb; tests exercise the parsers directly.
//
// Two dep forms only (D5/D9): `{ path = "..." }` or
// `{ git = "...", rev/branch = "..." }`. The two are mutually exclusive
// and rev/branch only mean anything on a git dep.

type projectManifest struct {
	Name         string                 `toml:"name"`
	Board        string                 `toml:"board"`
	Capabilities map[string]bool        `toml:"capabilities"`
	Deps         map[string]manifestDep `toml:"deps"`
}

type libraryManifest struct {
	Name      string                 `toml:"name"`
	Version   string                 `toml:"version"`
	Boards    []string               `toml:"boards"`
	Extension *manifestExtension     `toml:"extension"`
	Natives   []manifestNative       `toml:"natives"`
	Deps      map[string]manifestDep `toml:"deps"`
}

type manifestDep struct {
	Path   string `toml:"path"`
	Git    string `toml:"git"`
	Rev    string `toml:"rev"`
	Branch string `toml:"branch"`
}

type manifestExtension struct {
	Sources []string `toml:"sources"`
}

type manifestNative struct {
	Name      string `toml:"name"`
	Arity     int    `toml:"arity"`
	CFunction string `toml:"c_function"`
}

func parseProjectManifest(data []byte) (projectManifest, error) {
	var m projectManifest
	md, err := toml.Decode(string(data), &m)
	if err != nil {
		return projectManifest{}, fmt.Errorf("frothy.toml: %w", err)
	}
	if extra := md.Undecoded(); len(extra) > 0 {
		return projectManifest{}, fmt.Errorf("frothy.toml: unknown key %q", extra[0].String())
	}
	if m.Name == "" {
		return projectManifest{}, fmt.Errorf("frothy.toml: missing name")
	}
	if m.Board == "" {
		return projectManifest{}, fmt.Errorf("frothy.toml: missing board")
	}
	if err := validateCapabilities(m.Capabilities); err != nil {
		return projectManifest{}, fmt.Errorf("frothy.toml: %w", err)
	}
	for n, d := range m.Deps {
		if err := validateManifestDep(n, d); err != nil {
			return projectManifest{}, fmt.Errorf("frothy.toml: %w", err)
		}
	}
	return m, nil
}

func parseLibraryManifest(data []byte) (libraryManifest, error) {
	var m libraryManifest
	md, err := toml.Decode(string(data), &m)
	if err != nil {
		return libraryManifest{}, fmt.Errorf("lib.toml: %w", err)
	}
	if extra := md.Undecoded(); len(extra) > 0 {
		return libraryManifest{}, fmt.Errorf("lib.toml: unknown key %q", extra[0].String())
	}
	if m.Name == "" {
		return libraryManifest{}, fmt.Errorf("lib.toml: missing name")
	}
	if len(m.Boards) == 0 {
		return libraryManifest{}, fmt.Errorf("lib.toml %s: missing boards", m.Name)
	}
	if m.Extension != nil && len(m.Extension.Sources) == 0 {
		return libraryManifest{}, fmt.Errorf("lib.toml %s: [extension] declared without sources", m.Name)
	}
	if len(m.Natives) > 0 && m.Extension == nil {
		return libraryManifest{}, fmt.Errorf("lib.toml %s: [[natives]] without [extension]", m.Name)
	}
	for i, n := range m.Natives {
		if n.Name == "" {
			return libraryManifest{}, fmt.Errorf("lib.toml %s: native #%d missing name", m.Name, i+1)
		}
		if n.CFunction == "" {
			return libraryManifest{}, fmt.Errorf("lib.toml %s: native %s missing c_function", m.Name, n.Name)
		}
		if n.Arity < 0 || n.Arity > 255 {
			return libraryManifest{}, fmt.Errorf("lib.toml %s: native %s arity %d out of range", m.Name, n.Name, n.Arity)
		}
	}
	for n, d := range m.Deps {
		if err := validateManifestDep(n, d); err != nil {
			return libraryManifest{}, fmt.Errorf("lib.toml %s: %w", m.Name, err)
		}
	}
	return m, nil
}

func validateManifestDep(name string, d manifestDep) error {
	hasPath := d.Path != ""
	hasGit := d.Git != ""
	if !hasPath && !hasGit {
		return fmt.Errorf("dep %s: must declare path or git", name)
	}
	if hasPath && hasGit {
		return fmt.Errorf("dep %s: declares both path and git", name)
	}
	if hasPath && (d.Rev != "" || d.Branch != "") {
		return fmt.Errorf("dep %s: path dep cannot have rev or branch", name)
	}
	if hasGit && d.Branch != "" {
		return fmt.Errorf("dep %s: git dep must pin rev, not branch; add rev = \"<commit>\"", name)
	}
	if hasGit && d.Rev == "" {
		return fmt.Errorf("dep %s: git dep must pin rev; add rev = \"<commit>\"", name)
	}
	return nil
}
