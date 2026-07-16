package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

const sourcePlanByteLimit = 256 * 1024

type sourcePlanResult struct {
	Kind            string                 `json:"kind"`
	ResolverVersion int                    `json:"resolver_version,omitempty"`
	Board           string                 `json:"board,omitempty"`
	Sources         []sourcePlanSource     `json:"sources,omitempty"`
	Dependencies    []sourcePlanDependency `json:"dependencies,omitempty"`
	Message         string                 `json:"message,omitempty"`
}

type sourcePlanSource struct {
	Path   string `json:"path"`
	Source string `json:"source"`
}

type sourcePlanDependency struct {
	DeclaredBy string `json:"declared_by"`
	Name       string `json:"name"`
	Kind       string `json:"kind"`
	Git        string `json:"git"`
	Rev        string `json:"rev"`
}

func runSourcePlanCommand(args []string, stdout io.Writer, stderr io.Writer) int {
	fs := flag.NewFlagSet("frothy source-plan", flag.ContinueOnError)
	fs.SetOutput(stderr)
	projectDir := fs.String("project", ".", "project directory containing frothy.toml")
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if fs.NArg() != 0 {
		fmt.Fprintln(stderr, "frothy source-plan: unexpected positional argument(s)")
		return 2
	}

	absProject, err := filepath.Abs(*projectDir)
	if err != nil {
		writeSourcePlanResult(stdout, sourcePlanResult{Kind: "error", Message: err.Error()})
		return 1
	}
	result, err := resolveSourcePlan(absProject)
	if err != nil {
		writeSourcePlanResult(stdout, sourcePlanResult{Kind: "error", Message: err.Error()})
		return 1
	}
	writeSourcePlanResult(stdout, result)
	return 0
}

func resolveSourcePlan(projectDir string) (sourcePlanResult, error) {
	project, err := readProjectManifest(projectDir)
	if err != nil {
		return sourcePlanResult{}, err
	}

	var dependencies []sourcePlanDependency
	for _, name := range sortedDepNames(project.Deps) {
		dep := project.Deps[name]
		if dep.Path != "" {
			return sourcePlanResult{}, fmt.Errorf(
				"frothy.toml: path dependency %s is not available in the hosted editor", name)
		}
		dependencies = append(dependencies, sourcePlanDependency{
			DeclaredBy: "frothy.toml",
			Name:       name,
			Kind:       "git",
			Git:        dep.Git,
			Rev:        dep.Rev,
		})
	}
	if len(dependencies) > 0 {
		return sourcePlanResult{Kind: "needs_dependencies", Dependencies: dependencies}, nil
	}

	load, err := confinedSourceLoader(projectDir, sourcePlanByteLimit)
	if err != nil {
		return sourcePlanResult{}, err
	}
	mainSource, err := preprocessInclude("main.fr", load)
	if err != nil {
		return sourcePlanResult{}, fmt.Errorf("main.fr: %w", err)
	}
	return sourcePlanResult{
		Kind:            "resolved",
		ResolverVersion: 1,
		Board:           project.Board,
		Sources: []sourcePlanSource{{
			Path:   "main.fr",
			Source: mainSource,
		}},
	}, nil
}

func confinedSourceLoader(projectDir string, byteLimit int) (func(string) (string, error), error) {
	root, err := filepath.EvalSymlinks(projectDir)
	if err != nil {
		return nil, fmt.Errorf("project root: %w", err)
	}
	used := 0

	return func(path string) (string, error) {
		clean := filepath.Clean(path)
		if filepath.IsAbs(path) || clean == ".." || strings.HasPrefix(clean, ".."+string(filepath.Separator)) {
			return "", fmt.Errorf("include %q escapes the project", path)
		}
		if !strings.HasSuffix(clean, ".fr") {
			return "", fmt.Errorf("include %q must name a .fr file", path)
		}

		resolved, err := filepath.EvalSymlinks(filepath.Join(root, clean))
		if err != nil {
			if os.IsNotExist(err) {
				return "", fmt.Errorf("include %q does not exist", clean)
			}
			return "", fmt.Errorf("include %q is unavailable", clean)
		}
		relative, err := filepath.Rel(root, resolved)
		if err != nil || relative == ".." || strings.HasPrefix(relative, ".."+string(filepath.Separator)) {
			return "", fmt.Errorf("include %q escapes the project", clean)
		}
		info, err := os.Stat(resolved)
		if err != nil || !info.Mode().IsRegular() {
			return "", fmt.Errorf("include %q is not a regular file", clean)
		}
		remaining := byteLimit - used
		if info.Size() > int64(remaining) {
			return "", fmt.Errorf("resolved project exceeds %d source bytes", byteLimit)
		}
		data, err := os.ReadFile(resolved)
		if err != nil {
			return "", fmt.Errorf("include %q is unreadable", clean)
		}
		used += len(data)
		if used > byteLimit {
			return "", fmt.Errorf("resolved project exceeds %d source bytes", byteLimit)
		}
		return string(data), nil
	}, nil
}

func writeSourcePlanResult(output io.Writer, result sourcePlanResult) {
	encoder := json.NewEncoder(output)
	encoder.SetEscapeHTML(false)
	_ = encoder.Encode(result)
}
