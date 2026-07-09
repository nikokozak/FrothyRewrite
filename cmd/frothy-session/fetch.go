package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// frothy fetch resolves git deps into $FROTH_HOME/deps so build can use
// the same resolver as path deps. It deliberately does not own a registry,
// lockfile, semver, or branch refresh policy.

var errGitBinaryMissing = errors.New("git binary not found; install git and retry")

type gitCommandError struct {
	err error
}

func (e gitCommandError) Error() string { return e.err.Error() }

func (e gitCommandError) Unwrap() error { return e.err }

type fetchOptions struct {
	projectDir string
}

func runFetchMain() int {
	return runFetchCommand(os.Args[1:], os.Stdout, os.Stderr)
}

func runFetchCommand(args []string, stdout io.Writer, stderr io.Writer) int {
	fs := flag.NewFlagSet("frothy fetch", flag.ContinueOnError)
	fs.SetOutput(stderr)
	projectDir := fs.String("project", ".", "project directory containing frothy.toml")
	if helpRequested(args) {
		printVerbHelp(stdout, helpFor("fetch"), fs)
		return 0
	}
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if fs.NArg() != 0 {
		fmt.Fprintln(stderr, "frothy fetch: unexpected positional argument(s)")
		return 2
	}
	absProject, err := filepath.Abs(*projectDir)
	if err != nil {
		fmt.Fprintf(stderr, "frothy fetch: %v\n", err)
		return 1
	}
	if err := runFetch(fetchOptions{projectDir: absProject}); err != nil {
		fmt.Fprintf(stderr, "frothy fetch: %v\n", err)
		return 1
	}
	fmt.Fprintf(stdout, "frothy fetch: deps ready in %s\n", filepath.Join(frothHomeDir(), "deps"))
	return 0
}

func runFetch(opts fetchOptions) error {
	proj, err := readProjectManifest(opts.projectDir)
	if err != nil {
		return err
	}
	return fetchDeps(opts.projectDir, proj)
}

func fetchDeps(projectDir string, proj projectManifest) error {
	w := &fetchWalker{seen: map[string]bool{}}
	for _, name := range sortedDepNames(proj.Deps) {
		if err := w.walk(projectDir, name, proj.Deps[name]); err != nil {
			return err
		}
	}
	return nil
}

type fetchWalker struct {
	seen map[string]bool
}

func (w *fetchWalker) walk(parentDir, depName string, dep manifestDep) error {
	libPath, isGit, err := depLibraryPath(parentDir, dep)
	if err != nil {
		return fmt.Errorf("dep %s: %w", depName, err)
	}
	if isGit {
		if err := fetchGitDep(depName, dep, libPath); err != nil {
			return err
		}
	}
	if w.seen[libPath] {
		return nil
	}
	w.seen[libPath] = true

	var lib loadedLib
	if isGit {
		lib, err = loadGitLibrary(libPath)
	} else {
		lib, err = loadLibrary(libPath)
	}
	if err != nil {
		return fmt.Errorf("dep %s: %w", depName, err)
	}
	if lib.libManifest == nil {
		return nil
	}
	for _, name := range sortedDepNames(lib.libManifest.Deps) {
		if err := w.walk(libPath, name, lib.libManifest.Deps[name]); err != nil {
			return err
		}
	}
	return nil
}

func fetchGitDep(depName string, dep manifestDep, cacheDir string) error {
	if info, err := os.Stat(cacheDir); err == nil {
		if !info.IsDir() {
			return fmt.Errorf("dep %s: cache path %s is not a directory; remove it and run frothy fetch", depName, cacheDir)
		}
		if err := verifyCachedGitDep(cacheDir, dep.Rev); err != nil {
			return fmt.Errorf("dep %s: %w", depName, err)
		}
		return requireGitLibToml(depName, cacheDir)
	} else if !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("dep %s: %w", depName, err)
	}

	parent := filepath.Dir(cacheDir)
	if err := os.MkdirAll(parent, 0o755); err != nil {
		return fmt.Errorf("dep %s: %w", depName, err)
	}
	tmp, err := os.MkdirTemp(parent, ".fetch-")
	if err != nil {
		return fmt.Errorf("dep %s: %w", depName, err)
	}
	installed := false
	defer func() {
		if !installed {
			_ = os.RemoveAll(tmp)
		}
	}()

	if err := gitRun("clone", "--quiet", "--no-checkout", "--", dep.Git, tmp); err != nil {
		if errors.Is(err, errGitBinaryMissing) {
			return fmt.Errorf("dep %s: %w", depName, err)
		}
		return fmt.Errorf("dep %s: cannot clone %s; check network and repository URL", depName, dep.Git)
	}
	if err := gitRun("-C", tmp, "switch", "--quiet", "--detach", "--", dep.Rev); err != nil {
		if errors.Is(err, errGitBinaryMissing) {
			return fmt.Errorf("dep %s: %w", depName, err)
		}
		return fmt.Errorf("dep %s: rev %s not found in %s; pin a commit that exists", depName, dep.Rev, dep.Git)
	}
	if err := requireGitLibToml(depName, tmp); err != nil {
		return err
	}
	if err := os.Rename(tmp, cacheDir); err != nil {
		return fmt.Errorf("dep %s: cannot install cached checkout at %s: %w", depName, cacheDir, err)
	}
	installed = true
	return nil
}

func verifyCachedGitDep(cacheDir, rev string) error {
	head, err := gitOutput("-C", cacheDir, "rev-parse", "--verify", "HEAD^{commit}")
	if err != nil {
		if errors.Is(err, errGitBinaryMissing) {
			return err
		}
		return fmt.Errorf("cached checkout at %s is not readable; remove it and run frothy fetch", cacheDir)
	}
	want, err := gitOutput("-C", cacheDir, "rev-parse", "--verify", "--end-of-options", rev+"^{commit}")
	if err != nil {
		if errors.Is(err, errGitBinaryMissing) {
			return err
		}
		return fmt.Errorf("cached checkout at %s cannot resolve rev %s; remove it and run frothy fetch", cacheDir, rev)
	}
	if head != want {
		return fmt.Errorf("cached checkout at %s is not at rev %s; remove it and run frothy fetch", cacheDir, rev)
	}
	return nil
}

func requireGitLibToml(depName, dir string) error {
	if _, err := os.Stat(filepath.Join(dir, "lib.toml")); err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return fmt.Errorf("dep %s: git repo missing lib.toml at repo root", depName)
		}
		return fmt.Errorf("dep %s: cannot read lib.toml at repo root: %w", depName, err)
	}
	return nil
}

func gitRun(args ...string) error {
	_, err := gitOutput(args...)
	return err
}

func gitOutput(args ...string) (string, error) {
	gitPath, err := exec.LookPath("git")
	if err != nil {
		return "", errGitBinaryMissing
	}
	cmd := exec.Command(gitPath, args...)
	cmd.Env = append(os.Environ(),
		"GIT_PROTOCOL_FROM_USER=0",
		"GIT_ALLOW_PROTOCOL=https:http:ssh:git:file",
	)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return "", gitCommandError{err: err}
	}
	return strings.TrimSpace(string(out)), nil
}

func gitDepCacheDir(dep manifestDep) (string, error) {
	if dep.Rev == "" {
		return "", fmt.Errorf("git dep must pin rev; add rev = \"<commit>\"")
	}
	parts, err := gitDepCacheParts(dep.Git, dep.Rev)
	if err != nil {
		return "", err
	}
	return filepath.Join(append([]string{frothHomeDir(), "deps"}, parts...)...), nil
}

func frothHomeDir() string {
	if home := os.Getenv("FROTH_HOME"); home != "" {
		return home
	}
	return filepath.Join(os.Getenv("HOME"), ".froth")
}

func gitDepCacheParts(rawURL, rev string) ([]string, error) {
	host, repoPath, err := splitGitLocation(rawURL)
	if err != nil {
		return nil, err
	}
	segments := gitPathSegments(repoPath)
	if host == "" || len(segments) == 0 {
		return nil, fmt.Errorf("git URL %q must include host and repository path", rawURL)
	}
	parts := []string{cachePathSegment(host)}
	parts = append(parts, segments...)
	parts = append(parts, cachePathSegment(rev))
	return parts, nil
}

func splitGitLocation(raw string) (string, string, error) {
	if host, repoPath, ok := splitSCPGitLocation(raw); ok {
		return host, repoPath, nil
	}
	u, err := url.Parse(raw)
	if err != nil || u.Scheme == "" {
		if filepath.IsAbs(raw) {
			return "file", filepath.ToSlash(raw), nil
		}
		return "", "", fmt.Errorf("git URL %q must be an absolute URL or scp-style git URL", raw)
	}
	if u.Scheme == "file" {
		host := "file"
		if u.Host != "" {
			host = "file+" + u.Host
		}
		return host, u.Path, nil
	}
	return u.Host, u.Path, nil
}

func splitSCPGitLocation(raw string) (string, string, bool) {
	if strings.Contains(raw, "://") {
		return "", "", false
	}
	colon := strings.Index(raw, ":")
	slash := strings.Index(raw, "/")
	if colon <= 0 || (slash >= 0 && slash < colon) {
		return "", "", false
	}
	host := raw[:colon]
	if at := strings.LastIndex(host, "@"); at >= 0 {
		host = host[at+1:]
	}
	if host == "" || raw[colon+1:] == "" {
		return "", "", false
	}
	return host, raw[colon+1:], true
}

func gitPathSegments(repoPath string) []string {
	var segments []string
	for _, part := range strings.Split(strings.Trim(repoPath, "/"), "/") {
		if part == "" {
			continue
		}
		if unescaped, err := url.PathUnescape(part); err == nil {
			part = unescaped
		}
		segments = append(segments, part)
	}
	if len(segments) > 0 {
		segments[len(segments)-1] = strings.TrimSuffix(segments[len(segments)-1], ".git")
	}
	for i, part := range segments {
		segments[i] = cachePathSegment(part)
	}
	return segments
}

func cachePathSegment(s string) string {
	escaped := url.PathEscape(s)
	if escaped == "" || escaped == "." || escaped == ".." {
		return "_" + escaped
	}
	return escaped
}
