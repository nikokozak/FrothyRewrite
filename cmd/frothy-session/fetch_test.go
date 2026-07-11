package main

import (
	"bytes"
	"fmt"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestRunFetch_GitDepClonesPinnedRev(t *testing.T) {
	t.Setenv("FROTH_HOME", t.TempDir())
	remote, revs, _ := makeBareGitRepo(t, []map[string]string{
		{
			"lib.toml": `name = "servo"
boards = ["host"]
`,
			"lib.fr": "to servo.first [ ]\n",
		},
		{
			"lib.fr": "to servo.second [ ]\n",
		},
	})
	project := projectWithGitDep(t, remote, revs[0])

	var stdout, stderr bytes.Buffer
	if code := runFetchCommand([]string{"--project", project}, &stdout, &stderr); code != 0 {
		t.Fatalf("fetch exit = %d, stderr=%s", code, stderr.String())
	}

	cacheDir := mustGitCacheDir(t, remote, revs[0])
	if got := testGit(t, cacheDir, "rev-parse", "HEAD"); got != revs[0] {
		t.Fatalf("cache HEAD = %s, want %s", got, revs[0])
	}
	lib, err := os.ReadFile(filepath.Join(cacheDir, "lib.fr"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(lib), "servo.first") || strings.Contains(string(lib), "servo.second") {
		t.Fatalf("cached lib.fr = %q, want first commit content", string(lib))
	}
}

func TestGitDepCachePartsUseHostOwnerPathAndRev(t *testing.T) {
	parts, err := gitDepCacheParts("https://github.com/example/servo.git", "abc123")
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(parts, "/"), "github.com/example/servo/abc123"; got != want {
		t.Fatalf("cache parts = %q, want %q", got, want)
	}

	parts, err = gitDepCacheParts("git@github.com:example/servo.git", "abc123")
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(parts, "/"), "github.com/example/servo/abc123"; got != want {
		t.Fatalf("scp cache parts = %q, want %q", got, want)
	}
}

func TestRunFetch_CacheHitDoesNotReclone(t *testing.T) {
	t.Setenv("FROTH_HOME", t.TempDir())
	remote, revs, bare := makeBareGitRepo(t, []map[string]string{minimalGitLibrary("servo")})
	project := projectWithGitDep(t, remote, revs[0])

	var stdout, stderr bytes.Buffer
	if code := runFetchCommand([]string{"--project", project}, &stdout, &stderr); code != 0 {
		t.Fatalf("initial fetch exit = %d, stderr=%s", code, stderr.String())
	}
	cacheDir := mustGitCacheDir(t, remote, revs[0])
	sentinel := filepath.Join(cacheDir, "sentinel")
	writeFile(t, sentinel, "keep\n")
	if err := os.Rename(bare, bare+".offline"); err != nil {
		t.Fatal(err)
	}

	stdout.Reset()
	stderr.Reset()
	if code := runFetchCommand([]string{"--project", project}, &stdout, &stderr); code != 0 {
		t.Fatalf("warm fetch exit = %d, stderr=%s", code, stderr.String())
	}
	if _, err := os.Stat(sentinel); err != nil {
		t.Fatalf("cache hit recloned or removed sentinel: %v", err)
	}
}

func TestRunFetch_GitBinaryMissing(t *testing.T) {
	t.Setenv("FROTH_HOME", t.TempDir())
	t.Setenv("PATH", "")
	project := projectWithGitDep(t, "file:///missing.git", "abc123")

	var stdout, stderr bytes.Buffer
	if code := runFetchCommand([]string{"--project", project}, &stdout, &stderr); code == 0 {
		t.Fatal("expected fetch to fail without git")
	}
	if !strings.Contains(stderr.String(), "git binary not found; install git and retry") {
		t.Fatalf("stderr = %q", stderr.String())
	}
}

func TestRunFetch_GitDepRevNotFound(t *testing.T) {
	t.Setenv("FROTH_HOME", t.TempDir())
	remote, _, _ := makeBareGitRepo(t, []map[string]string{minimalGitLibrary("servo")})
	project := projectWithGitDep(t, remote, "deadbeef")

	var stdout, stderr bytes.Buffer
	if code := runFetchCommand([]string{"--project", project}, &stdout, &stderr); code == 0 {
		t.Fatal("expected fetch to fail on missing rev")
	}
	if !strings.Contains(stderr.String(), "rev deadbeef not found") ||
		!strings.Contains(stderr.String(), "pin a commit that exists") {
		t.Fatalf("stderr = %q", stderr.String())
	}
}

func TestRunFetch_GitDepMissingLibToml(t *testing.T) {
	t.Setenv("FROTH_HOME", t.TempDir())
	remote, revs, _ := makeBareGitRepo(t, []map[string]string{
		{"lib.fr": "to servo.use [ ]\n"},
	})
	project := projectWithGitDep(t, remote, revs[0])

	var stdout, stderr bytes.Buffer
	if code := runFetchCommand([]string{"--project", project}, &stdout, &stderr); code == 0 {
		t.Fatal("expected fetch to fail without lib.toml")
	}
	if !strings.Contains(stderr.String(), "dep servo: git repo missing lib.toml at repo root") {
		t.Fatalf("stderr = %q", stderr.String())
	}
}

func TestRunFetch_RecursiveExtProtocolDoesNotExecute(t *testing.T) {
	t.Setenv("FROTH_HOME", t.TempDir())
	sentinel := filepath.Join(t.TempDir(), "SENTINEL")
	maliciousURL := fmt.Sprintf("ext::sh -c 'touch %s'", sentinel)
	remote, revs, _ := makeBareGitRepo(t, []map[string]string{
		{
			"lib.toml": fmt.Sprintf(`name = "servo"
boards = ["host"]

[deps]
evil = { git = %q, rev = "abc123" }
`, maliciousURL),
			"lib.fr": "to servo.use [ ]\n",
		},
	})
	project := projectWithGitDep(t, remote, revs[0])

	var stdout, stderr bytes.Buffer
	if code := runFetchCommand([]string{"--project", project}, &stdout, &stderr); code == 0 {
		t.Fatal("expected fetch to reject ext:: recursive dep")
	}
	if !strings.Contains(stderr.String(), "dep evil: cannot clone ext::") {
		t.Fatalf("stderr = %q", stderr.String())
	}
	if _, err := os.Stat(sentinel); !os.IsNotExist(err) {
		t.Fatalf("ext:: command created sentinel or stat failed unexpectedly: %v", err)
	}
}

func TestFetchGitDepLeadingGitValueIsNotOption(t *testing.T) {
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git binary not available")
	}
	cacheDir := filepath.Join(t.TempDir(), "cache")

	err := fetchGitDep("servo", manifestDep{Git: "--help", Rev: "abc123"}, cacheDir)
	if err == nil {
		t.Fatal("expected leading git value to fail as a repository, not an option")
	}
	if !strings.Contains(err.Error(), "dep servo: cannot clone --help; check network and repository URL") {
		t.Fatalf("error = %q", err.Error())
	}
	if _, err := os.Stat(cacheDir); !os.IsNotExist(err) {
		t.Fatalf("cache dir exists after failed clone or stat failed unexpectedly: %v", err)
	}
}

func TestRunBuild_GitDepAutoFetches(t *testing.T) {
	t.Setenv("FROTH_HOME", t.TempDir())
	remote, revs, _ := makeBareGitRepo(t, []map[string]string{minimalGitLibrary("servo")})
	project := projectWithGitDep(t, remote, revs[0])

	var stdout, stderr bytes.Buffer
	if err := runBuild(buildOptions{projectDir: project, skipMake: true}, &stdout, &stderr); err != nil {
		t.Fatalf("runBuild: %v\nstderr: %s", err, stderr.String())
	}
	cacheDir := mustGitCacheDir(t, remote, revs[0])
	if _, err := os.Stat(cacheDir); err != nil {
		t.Fatalf("cache dir missing after build auto-fetch: %v", err)
	}
	library, err := os.ReadFile(filepath.Join(project, ".frothy", "build", "host", "library.fr"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(library), "servo.use") {
		t.Fatalf("library.fr = %q", string(library))
	}
}

func TestRunBuild_GitDepWarmCacheStaysOffline(t *testing.T) {
	t.Setenv("FROTH_HOME", t.TempDir())
	remote, revs, bare := makeBareGitRepo(t, []map[string]string{minimalGitLibrary("servo")})
	project := projectWithGitDep(t, remote, revs[0])

	var stdout, stderr bytes.Buffer
	if code := runFetchCommand([]string{"--project", project}, &stdout, &stderr); code != 0 {
		t.Fatalf("fetch exit = %d, stderr=%s", code, stderr.String())
	}
	if err := os.Rename(bare, bare+".offline"); err != nil {
		t.Fatal(err)
	}

	stdout.Reset()
	stderr.Reset()
	if err := runBuild(buildOptions{projectDir: project, skipMake: true}, &stdout, &stderr); err != nil {
		t.Fatalf("offline warm-cache build failed: %v\nstderr: %s", err, stderr.String())
	}
}

func TestRunBuild_GitDepRecursesThroughPathAndGitDeps(t *testing.T) {
	t.Setenv("FROTH_HOME", t.TempDir())
	helperRemote, helperRevs, _ := makeBareGitRepo(t, []map[string]string{minimalGitLibrary("helper")})
	servoRemote, servoRevs, _ := makeBareGitRepo(t, []map[string]string{
		{
			"lib.toml": fmt.Sprintf(`name = "servo"
boards = ["host"]

[deps]
helper = { git = %q, rev = %q }
local = { path = "local" }
`, helperRemote, helperRevs[0]),
			"lib.fr":         "to servo.use [ helper.use: local.use: ]\n",
			"local/lib.toml": "name = \"local\"\nboards = [\"host\"]\n",
			"local/lib.fr":   "to local.use [ ]\n",
		},
	})
	project := projectWithGitDep(t, servoRemote, servoRevs[0])

	var stdout, stderr bytes.Buffer
	if err := runBuild(buildOptions{projectDir: project, skipMake: true}, &stdout, &stderr); err != nil {
		t.Fatalf("runBuild: %v\nstderr: %s", err, stderr.String())
	}
	library, err := os.ReadFile(filepath.Join(project, ".frothy", "build", "host", "library.fr"))
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"to helper.use", "to local.use", "to servo.use"} {
		if !strings.Contains(string(library), want) {
			t.Fatalf("library.fr missing %s: %q", want, string(library))
		}
	}
}

func projectWithGitDep(t *testing.T, remote, rev string) string {
	t.Helper()
	return makeTempProject(t, fmt.Sprintf(`name = "blink"
board = "host"

[deps]
servo = { git = %q, rev = %q }
`, remote, rev), "", nil)
}

func minimalGitLibrary(name string) map[string]string {
	return map[string]string{
		"lib.toml": fmt.Sprintf("name = %q\nboards = [\"host\"]\n", name),
		"lib.fr":   fmt.Sprintf("to %s.use [ ]\n", name),
	}
}

func makeBareGitRepo(t *testing.T, commits []map[string]string) (string, []string, string) {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git binary not available")
	}
	root := t.TempDir()
	work := filepath.Join(root, "work")
	bare := filepath.Join(root, "remote.git")
	if err := os.MkdirAll(work, 0o755); err != nil {
		t.Fatal(err)
	}
	testGit(t, "", "init", "--quiet", work)
	var revs []string
	for i, files := range commits {
		for rel, body := range files {
			writeFile(t, filepath.Join(work, rel), body)
		}
		testGit(t, work, "add", ".")
		testGit(t, work, "-c", "user.name=Frothy Test", "-c", "user.email=frothy@example.test", "commit", "--quiet", "-m", fmt.Sprintf("commit %d", i+1))
		revs = append(revs, testGit(t, work, "rev-parse", "HEAD"))
	}
	testGit(t, "", "clone", "--bare", "--quiet", work, bare)
	return (&url.URL{Scheme: "file", Path: bare}).String(), revs, bare
}

func mustGitCacheDir(t *testing.T, remote, rev string) string {
	t.Helper()
	dir, err := gitDepCacheDir(manifestDep{Git: remote, Rev: rev})
	if err != nil {
		t.Fatal(err)
	}
	return dir
}

func testGit(t *testing.T, dir string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", args...)
	if dir != "" {
		cmd.Dir = dir
	}
	cmd.Env = append(os.Environ(),
		"GIT_AUTHOR_NAME=Frothy Test",
		"GIT_AUTHOR_EMAIL=frothy@example.test",
		"GIT_COMMITTER_NAME=Frothy Test",
		"GIT_COMMITTER_EMAIL=frothy@example.test",
		"GIT_TERMINAL_PROMPT=0",
	)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %s: %v\n%s", strings.Join(args, " "), err, string(out))
	}
	return strings.TrimSpace(string(out))
}
