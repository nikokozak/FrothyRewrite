package main

import (
	"fmt"
	"os"
	"path/filepath"
)

const frothySourceRootEnv = "FROTHY_SOURCE_ROOT"

func resolveFrothySourceRoot(start string) (string, error) {
	executable, _ := os.Executable()
	return resolveFrothySourceRootFrom(start, os.Getenv(frothySourceRootEnv), executable)
}

func resolveFrothySourceRootFrom(start, override, executable string) (string, error) {
	if override != "" {
		root := canonicalPath(override)
		if !isFrothySourceRoot(root) {
			return "", fmt.Errorf("%s=%q is not a Frothy source root (need Makefile, boards/, and src/froth.h)",
				frothySourceRootEnv, override)
		}
		return root, nil
	}

	if root := walkForFrothySourceRoot(start); root != "" {
		return root, nil
	}
	if executable != "" {
		if root := walkForFrothySourceRoot(filepath.Dir(canonicalPath(executable))); root != "" {
			return root, nil
		}
	}
	return "", fmt.Errorf("firmware commands require a Frothy source checkout; clone Frothy and run from it, or set %s", frothySourceRootEnv)
}

func walkForFrothySourceRoot(start string) string {
	for dir := canonicalPath(start); dir != ""; dir = filepath.Dir(dir) {
		if isFrothySourceRoot(dir) {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			break
		}
	}
	return ""
}

func isFrothySourceRoot(dir string) bool {
	return dir != "" &&
		fileExists(filepath.Join(dir, "Makefile")) &&
		dirExists(filepath.Join(dir, "boards")) &&
		fileExists(filepath.Join(dir, "src", "froth.h"))
}

func canonicalPath(path string) string {
	abs, err := filepath.Abs(path)
	if err != nil {
		return ""
	}
	if resolved, err := filepath.EvalSymlinks(abs); err == nil {
		abs = resolved
	}
	return filepath.Clean(abs)
}

func fileExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && !info.IsDir()
}

func dirExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.IsDir()
}
