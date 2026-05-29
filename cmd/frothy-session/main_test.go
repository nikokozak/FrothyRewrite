package main

import (
	"bytes"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

type fakeCompiler struct {
	target      compilerTarget
	targetErr   error
	targetCalls int
	results     []compileResult
	lines       []string
	commits     int
	drops       int
}

func (c *fakeCompiler) targetProfile() (compilerTarget, error) {
	c.targetCalls += 1
	if c.targetErr != nil {
		return compilerTarget{}, c.targetErr
	}
	return c.target, nil
}

func (c *fakeCompiler) compile(line string) (compileResult, error) {
	c.lines = append(c.lines, line)
	if len(c.results) == 0 {
		return compileResult{}, io.ErrUnexpectedEOF
	}
	result := c.results[0]
	c.results = c.results[1:]
	return result, nil
}

func (c *fakeCompiler) commit() error {
	c.commits += 1
	return nil
}

func (c *fakeCompiler) drop() error {
	c.drops += 1
	return nil
}

type fakeDevice struct {
	responses          []string
	responseErrs       []error
	interruptResponses []string
	interruptReadErrs  []error
	interruptErrs      []error
	sent               []string
	interrupts         int
	syncs              int
	onSend             func(line string)
	afterPrompt        func(line string)
}

func (d *fakeDevice) syncPrompt(timeout time.Duration) error {
	_ = timeout
	d.syncs += 1
	return nil
}

func (d *fakeDevice) sendLine(line string, timeout time.Duration, promptSeen func()) (string, error) {
	_ = timeout
	d.sent = append(d.sent, line)
	if d.onSend != nil {
		d.onSend(line)
	}
	if len(d.responses) == 0 {
		return "", io.ErrUnexpectedEOF
	}
	response := d.responses[0]
	d.responses = d.responses[1:]
	if len(d.responseErrs) == 0 {
		if promptSeen != nil {
			promptSeen()
		}
		if d.afterPrompt != nil {
			d.afterPrompt(line)
		}
		return response, nil
	}
	err := d.responseErrs[0]
	d.responseErrs = d.responseErrs[1:]
	if err == nil {
		if promptSeen != nil {
			promptSeen()
		}
		if d.afterPrompt != nil {
			d.afterPrompt(line)
		}
	}
	return response, err
}

func (d *fakeDevice) sendInterrupt() error {
	d.interrupts += 1
	if len(d.interruptErrs) == 0 {
		return nil
	}
	err := d.interruptErrs[0]
	d.interruptErrs = d.interruptErrs[1:]
	return err
}

func (d *fakeDevice) interrupt(timeout time.Duration) (string, error) {
	_ = timeout
	if err := d.sendInterrupt(); err != nil {
		return "", err
	}
	if len(d.interruptResponses) == 0 {
		return "", io.ErrUnexpectedEOF
	}
	response := d.interruptResponses[0]
	d.interruptResponses = d.interruptResponses[1:]
	if len(d.interruptReadErrs) == 0 {
		return response, nil
	}
	err := d.interruptReadErrs[0]
	d.interruptReadErrs = d.interruptReadErrs[1:]
	return response, err
}

func serialDeviceWithReadBytes(text string) *serialDevice {
	dev := &serialDevice{
		readCh: make(chan byte, len(text)),
		errCh:  make(chan error, 1),
	}
	for i := 0; i < len(text); i++ {
		dev.readCh <- text[i]
	}
	return dev
}

func TestSerialReadUntilPromptRequiresTerminalStatus(t *testing.T) {
	dev := serialDeviceWithReadBytes("> ok\n> ")

	response, err := dev.readUntilPrompt(time.Second, true, nil)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := response, "> ok\n"; got != want {
		t.Fatalf("response %q, want %q", got, want)
	}
}

func TestSerialReadUntilPromptAcceptsBareSyncPrompt(t *testing.T) {
	dev := serialDeviceWithReadBytes("> ")

	response, err := dev.readUntilPrompt(time.Second, false, nil)
	if err != nil {
		t.Fatal(err)
	}
	if response != "" {
		t.Fatalf("response %q, want empty sync response", response)
	}
}

func TestResponseOKUsesFinalStatusLine(t *testing.T) {
	tests := []struct {
		name     string
		response string
		want     bool
	}{
		{name: "plain ok", response: "ok\n", want: true},
		{name: "echoed apply ok", response: "apply 1234\r\nok\r\n", want: true},
		{name: "plain err", response: "err 9\n", want: false},
		{name: "echoed apply err", response: "apply 1234\r\nerr 9\r\n", want: false},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := responseOK(test.response); got != test.want {
				t.Fatalf("responseOK(%q) = %v, want %v", test.response, got,
					test.want)
			}
		})
	}
}

func statusResponse(compiler string) string {
	return "status\r\nfrothy status v1 profile=test profile_hash=1234abcd compiler=" +
		compiler + " names=device storage=volatile interrupt=cooperative word_size=16 int_min=-16384 int_max=16383 apply_bytes=92\r\nok\r\n"
}

func statusResponse32(compiler string) string {
	return "status\r\nfrothy status v1 profile=test32 profile_hash=bead1234 compiler=" +
		compiler + " names=device storage=volatile interrupt=cooperative word_size=32 int_min=-1073741824 int_max=1073741823 apply_bytes=128\r\nok\r\n"
}

func targetProfile(hash string) compilerTarget {
	return compilerTarget{
		profile:     "test",
		profileHash: hash,
		wordSize:    16,
		intMin:      -16384,
		intMax:      16383,
		applyBytes:  92,
	}
}

func targetProfile32(hash string) compilerTarget {
	return compilerTarget{
		profile:     "test32",
		profileHash: hash,
		wordSize:    32,
		intMin:      -1073741824,
		intMax:      1073741823,
		applyBytes:  128,
	}
}

func TestDefaultCompilerPathPrefersSiblingHelper(t *testing.T) {
	exe := filepath.Join(t.TempDir(), "frothy-session")
	helper := filepath.Join(filepath.Dir(exe), compilerProgramName)

	got := defaultCompilerPathFrom(
		func() (string, error) { return exe, nil },
		func(path string) (string, error) { return path, nil },
		func(path string) bool { return path == helper },
		func(string) (string, error) { return "", os.ErrNotExist },
	)
	if got != helper {
		t.Fatalf("default compiler path = %q, want %q", got, helper)
	}
}

func TestDefaultCompilerPathUsesResolvedLibexecHelper(t *testing.T) {
	prefix := t.TempDir()
	exe := filepath.Join(prefix, "bin", "frothy-session")
	resolvedExe := filepath.Join(prefix, "Cellar", "frothy", "0.1.0", "bin", "frothy-session")
	helper := filepath.Join(prefix, "Cellar", "frothy", "0.1.0", "libexec", "frothy", compilerProgramName)

	got := defaultCompilerPathFrom(
		func() (string, error) { return exe, nil },
		func(path string) (string, error) {
			if path != exe {
				t.Fatalf("resolved unexpected path %q", path)
			}
			return resolvedExe, nil
		},
		func(path string) bool { return path == helper },
		func(string) (string, error) { return "", os.ErrNotExist },
	)
	if got != helper {
		t.Fatalf("default compiler path = %q, want %q", got, helper)
	}
}

func TestDefaultCompilerPathFallsBackToLookPath(t *testing.T) {
	want := filepath.Join(t.TempDir(), compilerProgramName)

	got := defaultCompilerPathFrom(
		func() (string, error) { return "", os.ErrNotExist },
		func(path string) (string, error) { return path, nil },
		func(string) bool { return false },
		func(string) (string, error) { return want, nil },
	)
	if got != want {
		t.Fatalf("default compiler path = %q, want %q", got, want)
	}
}

func sessionStubVerbs() []verb {
	return []verb{{name: "session", summary: "stub", run: func() int { return 0 }}}
}

func TestFrothyHelpPrintsUsageToStdoutAndExitsZero(t *testing.T) {
	var stdout, stderr bytes.Buffer
	for _, flag := range []string{"--help", "-h", "help"} {
		stdout.Reset()
		stderr.Reset()
		code := runFrothyCommand([]string{"/usr/local/bin/frothy", flag}, &stdout, &stderr, sessionStubVerbs())
		if code != 0 {
			t.Fatalf("%s: exit code %d, want 0", flag, code)
		}
		if !strings.Contains(stdout.String(), "usage: frothy <verb>") {
			t.Fatalf("%s: stdout missing usage banner: %q", flag, stdout.String())
		}
		if !strings.Contains(stdout.String(), "session") {
			t.Fatalf("%s: stdout missing session verb: %q", flag, stdout.String())
		}
		if stderr.Len() != 0 {
			t.Fatalf("%s: stderr nonempty: %q", flag, stderr.String())
		}
	}
}

func TestFrothyWithoutVerbPrintsUsageToStderrAndExitsNonZero(t *testing.T) {
	var stdout, stderr bytes.Buffer
	code := runFrothyCommand([]string{"/usr/local/bin/frothy"}, &stdout, &stderr, sessionStubVerbs())
	if code == 0 {
		t.Fatal("exit code 0, want non-zero")
	}
	if stdout.Len() != 0 {
		t.Fatalf("stdout nonempty: %q", stdout.String())
	}
	if !strings.Contains(stderr.String(), "usage: frothy <verb>") {
		t.Fatalf("stderr missing usage banner: %q", stderr.String())
	}
}

func TestFrothyUnknownVerbPrintsUsageToStderrAndExitsNonZero(t *testing.T) {
	var stdout, stderr bytes.Buffer
	code := runFrothyCommand([]string{"/usr/local/bin/frothy", "bogus"}, &stdout, &stderr, sessionStubVerbs())
	if code == 0 {
		t.Fatal("exit code 0, want non-zero")
	}
	if stdout.Len() != 0 {
		t.Fatalf("stdout nonempty: %q", stdout.String())
	}
	if !strings.Contains(stderr.String(), `unknown verb "bogus"`) {
		t.Fatalf("stderr missing unknown verb message: %q", stderr.String())
	}
	if !strings.Contains(stderr.String(), "usage: frothy <verb>") {
		t.Fatalf("stderr missing usage banner: %q", stderr.String())
	}
}

func TestFrothyDispatchesSessionVerbAndRewritesArgs(t *testing.T) {
	savedArgs := os.Args
	defer func() { os.Args = savedArgs }()

	var ran bool
	verbs := []verb{{name: "session", summary: "stub", run: func() int { ran = true; return 0 }}}

	var stdout, stderr bytes.Buffer
	code := runFrothyCommand([]string{"/usr/local/bin/frothy", "session", "--dry-run"}, &stdout, &stderr, verbs)
	if code != 0 {
		t.Fatalf("exit code %d, want 0", code)
	}
	if !ran {
		t.Fatal("verb run was not called")
	}
	if got, want := strings.Join(os.Args, "\n"), "/usr/local/bin/frothy session\n--dry-run"; got != want {
		t.Fatalf("os.Args = %q, want %q", got, want)
	}
}

func TestFrothyDispatchPropagatesVerbExitCode(t *testing.T) {
	savedArgs := os.Args
	defer func() { os.Args = savedArgs }()

	verbs := []verb{{name: "session", summary: "stub", run: func() int { return 7 }}}

	var stdout, stderr bytes.Buffer
	code := runFrothyCommand([]string{"/usr/local/bin/frothy", "session"}, &stdout, &stderr, verbs)
	if code != 7 {
		t.Fatalf("exit code %d, want 7", code)
	}
}

func TestFrothySendDryRunCompilesFileToApplyAndRunLines(t *testing.T) {
	dir := t.TempDir()
	file := filepath.Join(dir, "src.frothy")
	if err := os.WriteFile(file, []byte("see 42\nlet x is 1\n"), 0644); err != nil {
		t.Fatal(err)
	}

	fake := &fakeCompiler{
		results: []compileResult{
			{action: actionSend, line: "see 42"},
			{action: actionApply, line: "apply x 01 00"},
		},
	}
	factory := func(_ string) (sessionCompiler, func(), error) {
		return fake, func() {}, nil
	}

	var stdout, stderr bytes.Buffer
	code := runSendCommand([]string{"--dry-run", file}, &stdout, &stderr, factory)
	if code != 0 {
		t.Fatalf("exit code %d, want 0; stderr=%q", code, stderr.String())
	}
	want := "see 42\napply x 01 00\n"
	if got := stdout.String(); got != want {
		t.Fatalf("stdout=%q, want %q", got, want)
	}
	if fake.commits != 1 {
		t.Fatalf("commits=%d, want 1", fake.commits)
	}
}

func TestFrothySendDryRunFlagAfterFile(t *testing.T) {
	dir := t.TempDir()
	file := filepath.Join(dir, "src.frothy")
	if err := os.WriteFile(file, []byte("see 42\n"), 0644); err != nil {
		t.Fatal(err)
	}

	fake := &fakeCompiler{
		results: []compileResult{{action: actionSend, line: "see 42"}},
	}
	factory := func(_ string) (sessionCompiler, func(), error) {
		return fake, func() {}, nil
	}

	var stdout, stderr bytes.Buffer
	code := runSendCommand([]string{file, "--dry-run"}, &stdout, &stderr, factory)
	if code != 0 {
		t.Fatalf("exit code %d, want 0; stderr=%q", code, stderr.String())
	}
	if got := stdout.String(); got != "see 42\n" {
		t.Fatalf("stdout=%q, want %q", got, "see 42\n")
	}
}

func TestFrothySendErrorsOnMissingFile(t *testing.T) {
	factory := func(_ string) (sessionCompiler, func(), error) {
		t.Fatal("compiler factory must not run when the file is missing")
		return nil, nil, nil
	}
	var stdout, stderr bytes.Buffer
	code := runSendCommand([]string{"--dry-run", "/nonexistent/missing-source.frothy"}, &stdout, &stderr, factory)
	if code == 0 {
		t.Fatal("exit code 0, want non-zero")
	}
	if !strings.Contains(stderr.String(), "send:") {
		t.Fatalf("stderr missing send: prefix: %q", stderr.String())
	}
	if !strings.Contains(stderr.String(), "missing-source.frothy") {
		t.Fatalf("stderr missing file path: %q", stderr.String())
	}
}

func TestFrothySendErrorsWhenPortMissingWithoutDryRun(t *testing.T) {
	factory := func(_ string) (sessionCompiler, func(), error) {
		t.Fatal("compiler factory must not run on the missing-port path")
		return nil, nil, nil
	}
	var stdout, stderr bytes.Buffer
	code := runSendCommand([]string{"some.frothy"}, &stdout, &stderr, factory)
	if code == 0 {
		t.Fatal("exit code 0, want non-zero")
	}
	if !strings.Contains(stderr.String(), "--port is required") {
		t.Fatalf("stderr missing --port required message: %q", stderr.String())
	}
}

func TestParseDeviceStatus(t *testing.T) {
	status, err := parseDeviceStatus(statusResponse("host-required"))
	if err != nil {
		t.Fatal(err)
	}
	if status.profile != "test" || status.profileHash != "1234abcd" ||
		status.compiler != compilerHostRequired || status.names != "device" ||
		status.storage != "volatile" || status.interrupt != "cooperative" ||
		status.wordSize != 16 || status.intMin != -16384 ||
		status.intMax != 16383 || status.applyBytes != 92 {
		t.Fatalf("status = %+v", status)
	}
}

func TestParseDeviceStatus32BitContract(t *testing.T) {
	status, err := parseDeviceStatus(statusResponse32("host-required"))
	if err != nil {
		t.Fatal(err)
	}
	if status.profile != "test32" || status.profileHash != "bead1234" ||
		status.compiler != compilerHostRequired || status.wordSize != 32 ||
		status.intMin != -1073741824 || status.intMax != 1073741823 ||
		status.applyBytes != 128 {
		t.Fatalf("status = %+v", status)
	}
}

func TestParseDeviceStatusRejectsUnknownCompiler(t *testing.T) {
	_, err := parseDeviceStatus(statusResponse("unknown"))
	if err == nil {
		t.Fatal("parseDeviceStatus accepted unknown compiler")
	}
}

func TestParseDeviceStatusRejectsUnknownVersion(t *testing.T) {
	response := strings.Replace(statusResponse("device"), "frothy status v1", "frothy status v2", 1)
	_, err := parseDeviceStatus(response)
	if err == nil {
		t.Fatal("parseDeviceStatus accepted unknown version")
	}
}

func TestParseDeviceStatusRejectsMissingRequiredField(t *testing.T) {
	response := strings.Replace(statusResponse("device"), " apply_bytes=92", "", 1)
	_, err := parseDeviceStatus(response)
	if err == nil {
		t.Fatal("parseDeviceStatus accepted missing apply_bytes")
	}
}

func TestParseDeviceStatusRejectsUnsupportedInterrupt(t *testing.T) {
	response := strings.Replace(statusResponse("device"), " interrupt=cooperative", " interrupt=none", 1)
	_, err := parseDeviceStatus(response)
	if err == nil {
		t.Fatal("parseDeviceStatus accepted unsupported interrupt mode")
	}
}

func TestParseDeviceStatusRejectsUnsupportedWordSize(t *testing.T) {
	response := strings.Replace(statusResponse("device"), " word_size=16", " word_size=64", 1)
	_, err := parseDeviceStatus(response)
	if err == nil {
		t.Fatal("parseDeviceStatus accepted unsupported word size")
	}
}

func TestParseDeviceStatusRejectsWrongIntRangeForWordSize(t *testing.T) {
	response := strings.Replace(statusResponse("device"), " int_min=-16384 int_max=16383", " int_min=-2048 int_max=2047", 1)
	_, err := parseDeviceStatus(response)
	if err == nil {
		t.Fatal("parseDeviceStatus accepted wrong int range")
	}
}

func TestParseCompilerTarget(t *testing.T) {
	target, err := parseCompilerTarget("target profile=test profile_hash=1234abcd word_size=16 int_min=-16384 int_max=16383 apply_bytes=92")
	if err != nil {
		t.Fatal(err)
	}
	if target.profile != "test" || target.profileHash != "1234abcd" ||
		target.wordSize != 16 || target.intMin != -16384 ||
		target.intMax != 16383 || target.applyBytes != 92 {
		t.Fatalf("target = %+v", target)
	}
}

func TestParseCompilerTarget32BitContract(t *testing.T) {
	target, err := parseCompilerTarget("target profile=test32 profile_hash=bead1234 word_size=32 int_min=-1073741824 int_max=1073741823 apply_bytes=128")
	if err != nil {
		t.Fatal(err)
	}
	if target.profile != "test32" || target.profileHash != "bead1234" ||
		target.wordSize != 32 || target.intMin != -1073741824 ||
		target.intMax != 1073741823 || target.applyBytes != 128 {
		t.Fatalf("target = %+v", target)
	}
}

func TestParseCompilerTargetRejectsBadHash(t *testing.T) {
	_, err := parseCompilerTarget("target profile=test profile_hash=not-hex word_size=16 int_min=-16384 int_max=16383 apply_bytes=92")
	if err == nil {
		t.Fatal("parseCompilerTarget accepted bad profile hash")
	}
}

func TestParseCompilerTargetRejectsUnsupportedWordSize(t *testing.T) {
	_, err := parseCompilerTarget("target profile=test profile_hash=1234abcd word_size=64 int_min=-16384 int_max=16383 apply_bytes=92")
	if err == nil {
		t.Fatal("parseCompilerTarget accepted unsupported word size")
	}
}

func TestParseCompilerTargetRejectsWrongIntRangeForWordSize(t *testing.T) {
	_, err := parseCompilerTarget("target profile=test profile_hash=1234abcd word_size=32 int_min=-16384 int_max=16383 apply_bytes=92")
	if err == nil {
		t.Fatal("parseCompilerTarget accepted wrong int range")
	}
}

func TestReadDeviceStatusRetriesPromptOnlyResponse(t *testing.T) {
	dev := &fakeDevice{responses: []string{"", statusResponse("host-required")}}

	status, err := readDeviceStatus(dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if status.compiler != compilerHostRequired {
		t.Fatalf("compiler = %s, want %s", status.compiler, compilerHostRequired)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nstatus"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	if dev.syncs != 2 {
		t.Fatalf("syncs=%d, want 2", dev.syncs)
	}
}

func TestReadDeviceStatusRetriesBareOKWithoutStatusLine(t *testing.T) {
	dev := &fakeDevice{responses: []string{"ok\n", statusResponse("device")}}

	status, err := readDeviceStatus(dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if status.compiler != compilerDevice {
		t.Fatalf("compiler = %s, want %s", status.compiler, compilerDevice)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nstatus"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	if dev.syncs != 2 {
		t.Fatalf("syncs=%d, want 2", dev.syncs)
	}
}

func TestReadDeviceStatusDoesNotRetryDeviceError(t *testing.T) {
	dev := &fakeDevice{responses: []string{"err 9\n", statusResponse("device")}}

	_, err := readDeviceStatus(dev, time.Second)
	if err == nil {
		t.Fatal("readDeviceStatus accepted device error")
	}
	if got, want := strings.Join(dev.sent, "\n"), "status"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	if dev.syncs != 1 {
		t.Fatalf("syncs=%d, want 1", dev.syncs)
	}
}

func TestVerifyCompilerTargetRejectsApplyBytesMismatch(t *testing.T) {
	comp := &fakeCompiler{target: compilerTarget{
		profile:     "test",
		profileHash: "1234abcd",
		wordSize:    16,
		intMin:      -16384,
		intMax:      16383,
		applyBytes:  60,
	}}
	status := deviceStatus{
		profile:     "test",
		profileHash: "1234abcd",
		compiler:    compilerHostRequired,
		wordSize:    16,
		intMin:      -16384,
		intMax:      16383,
		applyBytes:  92,
	}

	err := verifyCompilerTarget(comp, status)
	if err == nil {
		t.Fatal("verifyCompilerTarget accepted apply_bytes mismatch")
	}
}

func TestVerifyCompilerTargetRejectsWordSizeMismatch(t *testing.T) {
	comp := &fakeCompiler{target: compilerTarget{
		profile:     "test",
		profileHash: "1234abcd",
		wordSize:    32,
		intMin:      -1073741824,
		intMax:      1073741823,
		applyBytes:  92,
	}}
	status := deviceStatus{
		profile:     "test",
		profileHash: "1234abcd",
		compiler:    compilerHostRequired,
		wordSize:    16,
		intMin:      -16384,
		intMax:      16383,
		applyBytes:  92,
	}

	err := verifyCompilerTarget(comp, status)
	if err == nil {
		t.Fatal("verifyCompilerTarget accepted word_size mismatch")
	}
}

func TestVerifyCompilerTargetAccepts32BitContract(t *testing.T) {
	status, err := parseDeviceStatus(statusResponse32("host-required"))
	if err != nil {
		t.Fatal(err)
	}
	comp := &fakeCompiler{target: targetProfile32("bead1234")}

	if err := verifyCompilerTarget(comp, status); err != nil {
		t.Fatal(err)
	}
}

func TestIsBootDefinition(t *testing.T) {
	tests := []struct {
		line string
		want bool
	}{
		{line: "boot is fn [ one ]", want: true},
		{line: " \tboot\tis nil", want: true},
		{line: "boot:", want: false},
		{line: "reboot is fn [ one ]", want: false},
		{line: "boot", want: false},
	}

	for _, test := range tests {
		t.Run(test.line, func(t *testing.T) {
			if got := isBootDefinition(test.line); got != test.want {
				t.Fatalf("isBootDefinition(%q) = %v, want %v", test.line, got, test.want)
			}
		})
	}
}

func TestReadFileLinesMovesBootDefinitionsLast(t *testing.T) {
	path := filepath.Join(t.TempDir(), "program.fr")
	source := strings.Join([]string{
		"boot is fn [ blink: ]",
		"",
		"led is $led_builtin",
		"blink is fn [ pin: led, 1 ]",
		"  boot is fn [ one ]",
	}, "\n")
	if err := os.WriteFile(path, []byte(source), 0o644); err != nil {
		t.Fatal(err)
	}

	lines, err := readFileLines(path)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{
		"led is $led_builtin",
		"blink is fn [ pin: led, 1 ]",
		"boot is fn [ blink: ]",
		"boot is fn [ one ]",
	}
	if strings.Join(lines, "\n") != strings.Join(want, "\n") {
		t.Fatalf("readFileLines() = %#v, want %#v", lines, want)
	}
}

func TestReaderFromLines(t *testing.T) {
	reader := readerFromLines([]string{"one", "two"})
	bytes, err := io.ReadAll(reader)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := string(bytes), "one\ntwo\n"; got != want {
		t.Fatalf("readerFromLines() = %q, want %q", got, want)
	}
}

func TestSourceFormReaderPromptsAndGroupsMultilineTopForms(t *testing.T) {
	reader := newSourceFormReader(strings.NewReader("boot is fn [\n  one\n]\nwords\n"))
	var out strings.Builder

	source, ok, err := reader.next(&out, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !ok || source != "boot is fn [ one ]" {
		t.Fatalf("first source=%q ok=%v, want grouped boot form", source, ok)
	}

	source, ok, err = reader.next(&out, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !ok || source != "words" {
		t.Fatalf("second source=%q ok=%v, want words", source, ok)
	}
	if got, want := out.String(), "frothy> .. .. frothy> "; got != want {
		t.Fatalf("prompts %q, want %q", got, want)
	}
}

type chunkReader struct {
	chunks []string
	before []func()
	index  int
}

func (r *chunkReader) Read(p []byte) (int, error) {
	if r.index >= len(r.chunks) {
		return 0, io.EOF
	}
	if r.before != nil && r.before[r.index] != nil {
		r.before[r.index]()
	}
	chunk := r.chunks[r.index]
	r.index += 1
	return copy(p, chunk), nil
}

func TestSourceFormReaderCtrlCDropsPendingForm(t *testing.T) {
	tracker := &interruptTracker{}
	reader := newSourceFormReader(&chunkReader{
		chunks: []string{"boot is fn [\n", "words\n"},
		before: []func(){nil, tracker.request},
	})
	var out strings.Builder

	source, ok, err := reader.next(&out, tracker)
	if err != nil {
		t.Fatal(err)
	}
	if !ok || source != "words" {
		t.Fatalf("source=%q ok=%v, want next line as top form after Ctrl-C", source, ok)
	}
	if got, want := out.String(), "frothy> .. "; got != want {
		t.Fatalf("prompts %q, want %q", got, want)
	}
}

func TestReadFileLinesMovesMultilineBootDefinitionLast(t *testing.T) {
	path := filepath.Join(t.TempDir(), "program.fr")
	source := strings.Join([]string{
		"boot is fn [",
		"  blink:",
		"]",
		"led is $led_builtin",
		"blink is fn [ one ]",
	}, "\n")
	if err := os.WriteFile(path, []byte(source), 0o644); err != nil {
		t.Fatal(err)
	}

	lines, err := readFileLines(path)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{
		"led is $led_builtin",
		"blink is fn [ one ]",
		"boot is fn [ blink: ]",
	}
	if strings.Join(lines, "\n") != strings.Join(want, "\n") {
		t.Fatalf("readFileLines() = %#v, want %#v", lines, want)
	}
}

func TestSerialCompilesMultilineTopFormOnce(t *testing.T) {
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionApply, line: "apply 0102"},
		},
	}
	dev := &fakeDevice{responses: []string{statusResponse("host-required"), "ok\n"}}
	var out strings.Builder

	err := runSerial(strings.NewReader("boot is fn [\n  one\n]\n"), &out, comp, dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(comp.lines, "\n"), "boot is fn [ one ]"; got != want {
		t.Fatalf("compiled %q, want %q", got, want)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\napply 0102"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	if comp.commits != 1 || comp.drops != 0 {
		t.Fatalf("commits=%d drops=%d, want commits=1 drops=0", comp.commits, comp.drops)
	}
	if got, want := out.String(), "frothy> .. .. ok\nfrothy> "; got != want {
		t.Fatalf("output %q, want %q", got, want)
	}
}

func TestSerialApplyRejectDropsCompilerMirror(t *testing.T) {
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionApply, line: "apply 0102"},
		},
	}
	dev := &fakeDevice{responses: []string{statusResponse("host-required"), "err 9\n"}}
	var out strings.Builder

	err := runSerial(strings.NewReader("led is $led_builtin\n"), &out, comp, dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if comp.commits != 0 || comp.drops != 1 {
		t.Fatalf("commits=%d drops=%d, want commits=0 drops=1", comp.commits, comp.drops)
	}
	if comp.targetCalls != 1 {
		t.Fatalf("targetCalls=%d, want 1", comp.targetCalls)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\napply 0102"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialClearCommitFollowsDeviceOK(t *testing.T) {
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionClear, line: "clear"},
		},
	}
	dev := &fakeDevice{responses: []string{statusResponse("host-required"), "ok\n"}}
	var out strings.Builder

	err := runSerial(strings.NewReader("clear\n"), &out, comp, dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if comp.commits != 1 || comp.drops != 0 {
		t.Fatalf("commits=%d drops=%d, want commits=1 drops=0", comp.commits, comp.drops)
	}
	if comp.targetCalls != 1 {
		t.Fatalf("targetCalls=%d, want 1", comp.targetCalls)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nclear"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialDeviceCompilerModeSendsSourceWithoutHelper(t *testing.T) {
	dev := &fakeDevice{responses: []string{statusResponse("device"), "ok\n"}}
	var out strings.Builder

	err := runSerial(strings.NewReader("led is $led_builtin\n"), &out, nil, dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nled is $led_builtin"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialRefusesUnknownStatusBeforeCompiling(t *testing.T) {
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionApply, line: "apply 0102"},
		},
	}
	dev := &fakeDevice{responses: []string{statusResponse("unknown")}}
	var out strings.Builder

	err := runSerial(strings.NewReader("led is $led_builtin\n"), &out, comp, dev, time.Second)
	if err == nil {
		t.Fatal("runSerial accepted unknown compiler status")
	}
	if comp.targetCalls != 0 || len(comp.lines) != 0 || comp.commits != 0 || comp.drops != 0 {
		t.Fatalf("compiler used after bad status: %+v", comp)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialRefusesTargetHashMismatchBeforeCompiling(t *testing.T) {
	comp := &fakeCompiler{
		target: targetProfile("deadbeef"),
		results: []compileResult{
			{action: actionApply, line: "apply 0102"},
		},
	}
	dev := &fakeDevice{responses: []string{statusResponse("host-required")}}
	var out strings.Builder

	err := runSerial(strings.NewReader("led is $led_builtin\n"), &out, comp, dev, time.Second)
	if err == nil {
		t.Fatal("runSerial accepted mismatched compiler target")
	}
	if comp.targetCalls != 1 || len(comp.lines) != 0 || comp.commits != 0 || comp.drops != 0 {
		t.Fatalf("compiler advanced after target mismatch: %+v", comp)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialForegroundTimeoutInterruptsAndContinues(t *testing.T) {
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionSend, line: "run dead"},
			{action: actionSend, line: "run beef"},
		},
	}
	dev := &fakeDevice{
		responses: []string{
			statusResponse("host-required"),
			"err ",
			"ok\n",
		},
		responseErrs: []error{nil, errPromptTimeout, nil},
		interruptResponses: []string{
			"10\n",
		},
	}
	var out strings.Builder

	err := runSerial(strings.NewReader("forever [ 1 ]\nblink:\n"), &out, comp, dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if dev.interrupts != 1 {
		t.Fatalf("interrupts=%d, want 1", dev.interrupts)
	}
	if comp.commits != 0 || comp.drops != 0 {
		t.Fatalf("commits=%d drops=%d, want both 0", comp.commits, comp.drops)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nrun dead\nrun beef"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	if !strings.Contains(out.String(), "err 10\n") || !strings.Contains(out.String(), "ok\n") {
		t.Fatalf("output did not include recovery and next response: %q", out.String())
	}
}

func TestSerialForegroundTimeoutRequiresSettledRecovery(t *testing.T) {
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionSend, line: "run dead"},
			{action: actionSend, line: "run beef"},
		},
	}
	dev := &fakeDevice{
		responses: []string{
			statusResponse("host-required"),
			"",
		},
		responseErrs: []error{nil, errPromptTimeout},
		interruptResponses: []string{
			"boot banner\n",
		},
	}
	var out strings.Builder

	err := runSerial(strings.NewReader("forever [ 1 ]\nblink:\n"), &out, comp, dev, time.Second)
	if err == nil {
		t.Fatal("runSerial accepted unsettled interrupt recovery")
	}
	if dev.interrupts != 1 {
		t.Fatalf("interrupts=%d, want 1", dev.interrupts)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nrun dead"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	if len(comp.lines) != 1 {
		t.Fatalf("compiled lines=%q, want only first foreground line", strings.Join(comp.lines, "\n"))
	}
}

func TestSerialForegroundTimeoutFailsWhenInterruptDoesNotRecover(t *testing.T) {
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionSend, line: "run dead"},
			{action: actionSend, line: "run beef"},
		},
	}
	dev := &fakeDevice{
		responses: []string{
			statusResponse("host-required"),
			"tick\n",
		},
		responseErrs: []error{nil, errPromptTimeout},
		interruptResponses: []string{
			"",
		},
		interruptReadErrs: []error{
			errPromptTimeout,
		},
	}
	var out strings.Builder

	err := runSerial(strings.NewReader("forever [ 1 ]\nblink:\n"), &out, comp, dev, time.Second)
	if err == nil {
		t.Fatal("runSerial accepted failed interrupt recovery")
	}
	if !strings.Contains(err.Error(), "interrupt recovery failed") {
		t.Fatalf("error %q does not explain failed recovery", err)
	}
	if dev.interrupts != 1 {
		t.Fatalf("interrupts=%d, want 1", dev.interrupts)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nrun dead"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	if len(comp.lines) != 1 {
		t.Fatalf("compiled lines=%q, want only first foreground line", strings.Join(comp.lines, "\n"))
	}
}

func TestSerialSignalInterruptUsesTracker(t *testing.T) {
	tracker := &interruptTracker{}
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionSend, line: "run dead"},
			{action: actionSend, line: "run beef"},
		},
	}
	dev := &fakeDevice{
		responses: []string{
			"err 10\n",
			"ok\n",
		},
		onSend: func(line string) {
			if line == "run dead" {
				tracker.request()
			}
		},
	}
	var out strings.Builder

	err := runSerialWithModeAndInterrupts(strings.NewReader("forever [ 1 ]\nblink:\n"), &out, comp, dev, time.Second, true, tracker)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(dev.sent, "\n"), "run dead\nrun beef"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	if got, want := strings.Join(comp.lines, "\n"), "forever [ 1 ]\nblink:"; got != want {
		t.Fatalf("compiled %q, want %q", got, want)
	}
	if comp.commits != 0 || comp.drops != 0 {
		t.Fatalf("commits=%d drops=%d, want both 0", comp.commits, comp.drops)
	}
	if got, want := out.String(), "frothy> err 10\nfrothy> ok\nfrothy> "; got != want {
		t.Fatalf("output %q, want %q", got, want)
	}
}

func TestSerialSignalInterruptDuringUpdateStalesMirror(t *testing.T) {
	tests := []struct {
		name   string
		action compileAction
		line   string
		source string
	}{
		{name: "apply", action: actionApply, line: "apply 0102", source: "time is 200\n"},
		{name: "clear", action: actionClear, line: "clear", source: "clear\n"},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			tracker := &interruptTracker{}
			comp := &fakeCompiler{
				target: targetProfile("1234abcd"),
				results: []compileResult{
					{action: test.action, line: test.line},
				},
			}
			dev := &fakeDevice{
				responses: []string{
					"err 10\n",
				},
				onSend: func(line string) {
					if line == test.line {
						tracker.request()
					}
				},
			}
			var out strings.Builder

			err := runSerialWithModeAndInterrupts(strings.NewReader(test.source), &out, comp, dev, time.Second, true, tracker)
			if err == nil {
				t.Fatalf("runSerialWithModeAndInterrupts accepted interrupted %s", test.name)
			}
			if !strings.Contains(err.Error(), "compiler mirror stale") {
				t.Fatalf("error %q does not explain stale mirror", err)
			}
			if comp.commits != 0 || comp.drops != 1 {
				t.Fatalf("commits=%d drops=%d, want commits=0 drops=1", comp.commits, comp.drops)
			}
			if got, want := strings.Join(dev.sent, "\n"), test.line; got != want {
				t.Fatalf("sent %q, want %q", got, want)
			}
			if got, want := out.String(), "frothy> err 10\n"; got != want {
				t.Fatalf("output %q, want %q", got, want)
			}
		})
	}
}

func TestSerialLateSignalAfterAcceptedUpdateCommits(t *testing.T) {
	tests := []struct {
		name   string
		action compileAction
		line   string
		source string
	}{
		{name: "apply", action: actionApply, line: "apply 0102", source: "time is 200\n"},
		{name: "clear", action: actionClear, line: "clear", source: "clear\n"},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			tracker := &interruptTracker{}
			comp := &fakeCompiler{
				target: targetProfile("1234abcd"),
				results: []compileResult{
					{action: test.action, line: test.line},
				},
			}
			var dev *fakeDevice
			dev = &fakeDevice{
				responses: []string{
					"ok\n",
				},
				afterPrompt: func(line string) {
					if line == test.line && tracker.requestAndShouldForward() {
						_ = dev.sendInterrupt()
					}
				},
			}
			var out strings.Builder

			err := runSerialWithModeAndInterrupts(strings.NewReader(test.source), &out, comp, dev, time.Second, true, tracker)
			if err != nil {
				t.Fatal(err)
			}
			if comp.commits != 1 || comp.drops != 0 {
				t.Fatalf("commits=%d drops=%d, want commits=1 drops=0", comp.commits, comp.drops)
			}
			if dev.interrupts != 0 {
				t.Fatalf("interrupts=%d, want 0", dev.interrupts)
			}
			if got, want := strings.Join(dev.sent, "\n"), test.line; got != want {
				t.Fatalf("sent %q, want %q", got, want)
			}
			if got, want := out.String(), "frothy> ok\nfrothy> "; got != want {
				t.Fatalf("output %q, want %q", got, want)
			}
		})
	}
}

func TestSerialUpdateTimeoutDropsMirrorAndStopsStale(t *testing.T) {
	tests := []struct {
		name   string
		action compileAction
		line   string
	}{
		{name: "apply", action: actionApply, line: "apply 0102"},
		{name: "clear", action: actionClear, line: "clear"},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			comp := &fakeCompiler{
				target: targetProfile("1234abcd"),
				results: []compileResult{
					{action: test.action, line: test.line},
					{action: actionSend, line: "run beef"},
				},
			}
			dev := &fakeDevice{
				responses: []string{
					statusResponse("host-required"),
					"",
				},
				responseErrs: []error{nil, errPromptTimeout},
				interruptResponses: []string{
					"err 10\n",
				},
			}
			var out strings.Builder

			err := runSerial(strings.NewReader("blink is fn [ forever [ 1 ] ]\nblink:\n"), &out, comp, dev, time.Second)
			if err == nil {
				t.Fatal("runSerial accepted timeout during mirror-affecting update")
			}
			if !strings.Contains(err.Error(), "compiler mirror stale") {
				t.Fatalf("error %q does not explain stale mirror", err)
			}
			if comp.commits != 0 || comp.drops != 1 {
				t.Fatalf("commits=%d drops=%d, want commits=0 drops=1", comp.commits, comp.drops)
			}
			if dev.interrupts != 1 {
				t.Fatalf("interrupts=%d, want 1", dev.interrupts)
			}
			if got, want := strings.Join(dev.sent, "\n"), "status\n"+test.line; got != want {
				t.Fatalf("sent %q, want %q", got, want)
			}
			if len(comp.lines) != 1 {
				t.Fatalf("compiled lines=%q, want only first update line", strings.Join(comp.lines, "\n"))
			}
		})
	}
}

func TestPromptRecoveredAfterInterrupt(t *testing.T) {
	if !responseSettledAfterInterrupt("o" + "k\n") {
		t.Fatal("responseSettledAfterInterrupt did not combine partial ok")
	}
	if !responseSettledAfterInterrupt("noise\nerr " + "10\n") {
		t.Fatal("responseSettledAfterInterrupt did not combine partial interrupt")
	}
	if !responseSettledAfterInterrupt("tick\ntick\nti" + "ck\nerr 10\n") {
		t.Fatal("responseSettledAfterInterrupt did not handle interrupted output before status")
	}
	if !promptRecoveredAfterInterrupt("tic", "err 10\n") {
		t.Fatal("promptRecoveredAfterInterrupt let partial output poison recovery")
	}
	if !promptRecoveredAfterInterrupt("err ", "10\n") {
		t.Fatal("promptRecoveredAfterInterrupt did not accept split status")
	}
}

func decodeRecords(t *testing.T, text string) []map[string]any {
	t.Helper()
	lines := strings.Split(strings.TrimSpace(text), "\n")
	records := make([]map[string]any, 0, len(lines))
	for _, line := range lines {
		if line == "" {
			continue
		}
		var record map[string]any
		if err := json.Unmarshal([]byte(line), &record); err != nil {
			t.Fatalf("record %q is not valid JSON: %v", line, err)
		}
		validateRecordEnvelope(t, line, record, len(records)+1)
		records = append(records, record)
	}
	return records
}

func validateRecordEnvelope(t *testing.T, line string, record map[string]any, wantSeq int) {
	t.Helper()
	if record["v"] != float64(1) {
		t.Fatalf("record %q has v=%#v, want 1", line, record["v"])
	}
	if record["seq"] != float64(wantSeq) {
		t.Fatalf("record %q has seq=%#v, want %d", line, record["seq"], wantSeq)
	}
	if requiredRecordString(t, line, record, "session") == "" {
		t.Fatalf("record %q has empty session", line)
	}
	requiredRecordString(t, line, record, "kind")
	requiredRecordString(t, line, record, "state")
	requiredRecordString(t, line, record, "mirror")
}

func requiredRecordString(t *testing.T, line string, record map[string]any, field string) string {
	t.Helper()
	value, ok := record[field].(string)
	if !ok {
		t.Fatalf("record %q has %s=%#v, want string", line, field, record[field])
	}
	return value
}

func recordKinds(records []map[string]any) string {
	kinds := make([]string, 0, len(records))
	for _, record := range records {
		kinds = append(kinds, record["kind"].(string))
	}
	return strings.Join(kinds, ",")
}

func recordWithKind(records []map[string]any, kind string) map[string]any {
	for _, record := range records {
		if record["kind"] == kind {
			return record
		}
	}
	return nil
}

func runRecordsTestSession(t *testing.T, input io.Reader, output io.Writer, comp sessionCompiler, dev sessionDevice, timeout time.Duration, hostCompile bool, tracker *interruptTracker) error {
	t.Helper()
	records := newRecordWriter(output, "s1")
	if err := records.sessionStart(); err != nil {
		t.Fatal(err)
	}
	status, err := readDeviceStatus(dev, timeout)
	if err != nil {
		_ = records.sessionError(recordStateError, recordMirrorNone, recordErrorStatusFailed, err.Error())
		return err
	}
	useCompiler, err := status.useHostCompiler(hostCompile)
	if err != nil {
		_ = records.sessionError(recordStateError, recordMirrorNone, recordErrorStatusFailed, err.Error())
		return err
	}
	if useCompiler {
		if err := verifyCompilerTarget(comp, status); err != nil {
			_ = records.sessionError(recordStateError, recordMirrorClean, recordErrorHelperFailed, err.Error())
			return err
		}
	}
	if err := records.status(status, useCompiler); err != nil {
		t.Fatal(err)
	}
	return runSerialRecordsWithMode(input, records, comp, dev, timeout, useCompiler, tracker)
}

func TestOpenRecordOutputWritesTranscriptCopy(t *testing.T) {
	path := filepath.Join(t.TempDir(), "session.ndjson")
	var stdout strings.Builder
	output, closeOutput, err := openRecordOutput(&stdout, path)
	if err != nil {
		t.Fatal(err)
	}

	records := newRecordWriter(output, "s1")
	if err := records.sessionStart(); err != nil {
		t.Fatal(err)
	}
	if err := records.sessionEnd(recordMirrorNone); err != nil {
		t.Fatal(err)
	}
	if err := closeOutput(); err != nil {
		t.Fatal(err)
	}

	transcript, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := string(transcript), stdout.String(); got != want {
		t.Fatalf("transcript does not match stdout\ngot:\n%s\nwant:\n%s", got, want)
	}

	decoded := decodeRecords(t, stdout.String())
	if got, want := recordKinds(decoded), "session_start,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
}

func TestReplayLinesFromTranscriptKeepsOnlyAcceptedSource(t *testing.T) {
	transcript := strings.Join([]string{
		`{"v":1,"session":"s1","seq":1,"kind":"session_start","state":"syncing","mirror":"none"}`,
		`{"v":1,"session":"s1","seq":2,"kind":"status","state":"idle","mirror":"none"}`,
		`{"v":1,"session":"s1","seq":3,"kind":"send","state":"waiting","mirror":"none","source":"led is $led_builtin","line":"led is $led_builtin","action":"direct"}`,
		`{"v":1,"session":"s1","seq":4,"kind":"response","state":"idle","mirror":"none","status":"ok","ok":true,"text":"ok\n"}`,
		`{"v":1,"session":"s1","seq":5,"kind":"compile_error","state":"idle","mirror":"none","source":"bad is fn [ pin: ]","reason":"source","status":"err 4","text":"err 4\n"}`,
		`{"v":1,"session":"s1","seq":6,"kind":"send","state":"waiting","mirror":"none","source":"bad:","line":"bad:","action":"direct"}`,
		`{"v":1,"session":"s1","seq":7,"kind":"response","state":"idle","mirror":"none","status":"err 9","ok":false,"text":"err 9\n"}`,
		`{"v":1,"session":"s1","seq":8,"kind":"send","state":"waiting","mirror":"none","source":"forever [ 1 ]","line":"forever [ 1 ]","action":"direct"}`,
		`{"v":1,"session":"s1","seq":9,"kind":"interrupt","state":"idle","mirror":"none","settled":true,"status":"err 10","text":"err 10\n"}`,
		`{"v":1,"session":"s1","seq":10,"kind":"future_observation","state":"idle","mirror":"none"}`,
		`{"v":1,"session":"s1","seq":11,"kind":"session_end","state":"closed","mirror":"none"}`,
	}, "\n") + "\n"

	lines, err := replayLinesFromTranscript(strings.NewReader(transcript))
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(lines, "\n"), "led is $led_builtin"; got != want {
		t.Fatalf("replay lines %q, want %q", got, want)
	}
}

func TestReplayLinesFromTranscriptRejectsBadRecords(t *testing.T) {
	tests := []struct {
		name       string
		transcript string
		wantError  string
	}{
		{
			name:       "malformed JSON",
			transcript: `{"v":1,` + "\n",
			wantError:  "malformed JSON",
		},
		{
			name:       "empty transcript",
			transcript: "",
			wantError:  "transcript is empty",
		},
		{
			name: "unsupported version",
			transcript: strings.Join([]string{
				`{"v":2,"session":"s1","seq":1,"kind":"session_start","state":"syncing","mirror":"none"}`,
			}, "\n") + "\n",
			wantError: "unsupported record version",
		},
		{
			name: "missing send source",
			transcript: strings.Join([]string{
				`{"v":1,"session":"s1","seq":1,"kind":"session_start","state":"syncing","mirror":"none"}`,
				`{"v":1,"session":"s1","seq":2,"kind":"send","state":"waiting","mirror":"none","line":"words","action":"direct"}`,
			}, "\n") + "\n",
			wantError: "missing source",
		},
		{
			name: "stale terminal state",
			transcript: strings.Join([]string{
				`{"v":1,"session":"s1","seq":1,"kind":"session_start","state":"syncing","mirror":"none"}`,
				`{"v":1,"session":"s1","seq":2,"kind":"interrupt","state":"stale","mirror":"stale","settled":true,"status":"err 10","text":"err 10\n"}`,
			}, "\n") + "\n",
			wantError: "stale",
		},
		{
			name: "incomplete sent source",
			transcript: strings.Join([]string{
				`{"v":1,"session":"s1","seq":1,"kind":"session_start","state":"syncing","mirror":"none"}`,
				`{"v":1,"session":"s1","seq":2,"kind":"send","state":"waiting","mirror":"none","source":"words","line":"words","action":"direct"}`,
			}, "\n") + "\n",
			wantError: "ended before sent source settled",
		},
		{
			name: "session error",
			transcript: strings.Join([]string{
				`{"v":1,"session":"s1","seq":1,"kind":"session_start","state":"syncing","mirror":"none"}`,
				`{"v":1,"session":"s1","seq":2,"kind":"session_error","state":"idle","mirror":"none","code":"device_lost","message":"lost"}`,
			}, "\n") + "\n",
			wantError: "session_error",
		},
		{
			name: "double send",
			transcript: strings.Join([]string{
				`{"v":1,"session":"s1","seq":1,"kind":"session_start","state":"syncing","mirror":"none"}`,
				`{"v":1,"session":"s1","seq":2,"kind":"send","state":"waiting","mirror":"none","source":"one","line":"one","action":"direct"}`,
				`{"v":1,"session":"s1","seq":3,"kind":"send","state":"waiting","mirror":"none","source":"two","line":"two","action":"direct"}`,
			}, "\n") + "\n",
			wantError: "send before previous source settled",
		},
		{
			name: "sequence gap",
			transcript: strings.Join([]string{
				`{"v":1,"session":"s1","seq":1,"kind":"session_start","state":"syncing","mirror":"none"}`,
				`{"v":1,"session":"s1","seq":3,"kind":"session_end","state":"closed","mirror":"none"}`,
			}, "\n") + "\n",
			wantError: "want 2",
		},
		{
			name: "mixed sessions",
			transcript: strings.Join([]string{
				`{"v":1,"session":"s1","seq":1,"kind":"session_start","state":"syncing","mirror":"none"}`,
				`{"v":1,"session":"s2","seq":2,"kind":"session_end","state":"closed","mirror":"none"}`,
			}, "\n") + "\n",
			wantError: "changed session",
		},
		{
			name: "unknown state",
			transcript: strings.Join([]string{
				`{"v":1,"session":"s1","seq":1,"kind":"session_start","state":"strange","mirror":"none"}`,
			}, "\n") + "\n",
			wantError: "unknown state",
		},
		{
			name: "unknown mirror",
			transcript: strings.Join([]string{
				`{"v":1,"session":"s1","seq":1,"kind":"session_start","state":"syncing","mirror":"strange"}`,
			}, "\n") + "\n",
			wantError: "unknown mirror",
		},
		{
			name: "no clean end",
			transcript: strings.Join([]string{
				`{"v":1,"session":"s1","seq":1,"kind":"session_start","state":"syncing","mirror":"none"}`,
				`{"v":1,"session":"s1","seq":2,"kind":"status","state":"idle","mirror":"none"}`,
			}, "\n") + "\n",
			wantError: "did not end cleanly",
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			_, err := replayLinesFromTranscript(strings.NewReader(test.transcript))
			if err == nil {
				t.Fatalf("accepted bad transcript")
			}
			if !strings.Contains(err.Error(), test.wantError) {
				t.Fatalf("error %q does not contain %q", err, test.wantError)
			}
		})
	}
}

func TestReplayLinesFromWrittenRecords(t *testing.T) {
	var out strings.Builder
	records := newRecordWriter(&out, "s1")
	if err := records.sessionStart(); err != nil {
		t.Fatal(err)
	}
	status, err := parseDeviceStatus(statusResponse("device"))
	if err != nil {
		t.Fatal(err)
	}
	if err := records.status(status, false); err != nil {
		t.Fatal(err)
	}
	if err := records.send("words", compileResult{action: actionPass, line: "words"}, recordMirrorNone); err != nil {
		t.Fatal(err)
	}
	if err := records.response("ok\n", recordMirrorNone, ""); err != nil {
		t.Fatal(err)
	}
	if err := records.sessionEnd(recordMirrorNone); err != nil {
		t.Fatal(err)
	}

	lines, err := replayLinesFromTranscript(strings.NewReader(out.String()))
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(lines, "\n"), "words"; got != want {
		t.Fatalf("replay lines %q, want %q", got, want)
	}
}

func TestReplayLinesUseNormalSerialSessionPath(t *testing.T) {
	transcript := strings.Join([]string{
		`{"v":1,"session":"s1","seq":1,"kind":"session_start","state":"syncing","mirror":"none"}`,
		`{"v":1,"session":"s1","seq":2,"kind":"status","state":"idle","mirror":"clean"}`,
		`{"v":1,"session":"s1","seq":3,"kind":"send","state":"waiting","mirror":"pending","source":"time is 200","line":"apply 0102","action":"apply"}`,
		`{"v":1,"session":"s1","seq":4,"kind":"response","state":"idle","mirror":"clean","status":"ok","ok":true,"text":"ok\n","mirror_action":"commit"}`,
		`{"v":1,"session":"s1","seq":5,"kind":"send","state":"waiting","mirror":"clean","source":"blink:","line":"run dead","action":"eval"}`,
		`{"v":1,"session":"s1","seq":6,"kind":"response","state":"idle","mirror":"clean","status":"ok","ok":true,"text":"ok\n"}`,
		`{"v":1,"session":"s1","seq":7,"kind":"session_end","state":"closed","mirror":"clean"}`,
	}, "\n") + "\n"
	lines, err := replayLinesFromTranscript(strings.NewReader(transcript))
	if err != nil {
		t.Fatal(err)
	}

	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionApply, line: "apply 0102"},
			{action: actionSend, line: "run dead"},
		},
	}
	dev := &fakeDevice{
		responses: []string{
			statusResponse("host-required"),
			"ok\n",
			"ok\n",
		},
	}
	var out strings.Builder

	err = runSerial(readerFromLines(lines), &out, comp, dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(comp.lines, "\n"), "time is 200\nblink:"; got != want {
		t.Fatalf("compiled %q, want %q", got, want)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\napply 0102\nrun dead"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	if comp.commits != 1 || comp.drops != 0 {
		t.Fatalf("commits=%d drops=%d, want commits=1 drops=0", comp.commits, comp.drops)
	}
	if got, want := out.String(), "frothy> ok\nfrothy> ok\nfrothy> "; got != want {
		t.Fatalf("output %q, want %q", got, want)
	}
}

func TestSameCleanPath(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "session.ndjson")
	if err := os.WriteFile(path, []byte("[]"), 0o644); err != nil {
		t.Fatal(err)
	}
	same, err := sameCleanPath(path, filepath.Join(dir, ".", "session.ndjson"))
	if err != nil {
		t.Fatal(err)
	}
	if !same {
		t.Fatal("sameCleanPath did not match cleaned paths")
	}
	linkDir := filepath.Join(t.TempDir(), "linked")
	if err := os.Symlink(dir, linkDir); err == nil {
		symlinkSame, err := sameCleanPath(path, filepath.Join(linkDir, "session.ndjson"))
		if err != nil {
			t.Fatal(err)
		}
		if !symlinkSame {
			t.Fatal("sameCleanPath did not match symlinked path")
		}
	} else {
		t.Logf("skipping symlink path check: %v", err)
	}

	other, err := sameCleanPath(path, filepath.Join(dir, "replay.ndjson"))
	if err != nil {
		t.Fatal(err)
	}
	if other {
		t.Fatal("sameCleanPath matched different paths")
	}
}

func TestValidateSessionOptionsRejectsReplayConflicts(t *testing.T) {
	dir := t.TempDir()
	replay := filepath.Join(dir, "session.ndjson")
	other := filepath.Join(dir, "other.ndjson")
	tests := []struct {
		name       string
		filePath   string
		dryRun     bool
		records    bool
		transcript string
		replay     string
		wantCode   int
		wantError  string
	}{
		{
			name:      "records dry run",
			dryRun:    true,
			records:   true,
			wantCode:  2,
			wantError: "--records cannot be combined with --dry-run",
		},
		{
			name:       "transcript without records",
			transcript: other,
			wantCode:   2,
			wantError:  "--transcript requires --records",
		},
		{
			name:      "replay file",
			filePath:  "program.fr",
			replay:    replay,
			wantCode:  2,
			wantError: "--replay cannot be combined with --file",
		},
		{
			name:      "replay dry run",
			dryRun:    true,
			replay:    replay,
			wantCode:  2,
			wantError: "--replay cannot be combined with --dry-run",
		},
		{
			name:       "same replay transcript",
			records:    true,
			transcript: filepath.Join(dir, ".", "session.ndjson"),
			replay:     replay,
			wantCode:   2,
			wantError:  "--replay cannot write --transcript to the same path",
		},
		{
			name:       "different replay transcript",
			records:    true,
			transcript: other,
			replay:     replay,
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			code, err := validateSessionOptions(test.filePath, test.dryRun, test.records, test.transcript, test.replay)
			if test.wantError == "" {
				if err != nil || code != 0 {
					t.Fatalf("validateSessionOptions() = code %d err %v, want success", code, err)
				}
				return
			}
			if err == nil {
				t.Fatal("validateSessionOptions accepted invalid flags")
			}
			if code != test.wantCode {
				t.Fatalf("exit code %d, want %d", code, test.wantCode)
			}
			if !strings.Contains(err.Error(), test.wantError) {
				t.Fatalf("error %q does not contain %q", err, test.wantError)
			}
		})
	}
}

func TestReadReplayLinesReturnsOpenError(t *testing.T) {
	_, err := readReplayLines(filepath.Join(t.TempDir(), "missing.ndjson"))
	if err == nil {
		t.Fatal("readReplayLines accepted missing file")
	}
}

func TestRecordsAcceptedApplyCommit(t *testing.T) {
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionApply, line: "apply 0102"},
		},
	}
	dev := &fakeDevice{responses: []string{statusResponse("host-required"), "ok\n"}}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("time is 200\n"), &out, comp, dev, time.Second, false, &interruptTracker{})
	if err != nil {
		t.Fatal(err)
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,send,response,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	status := recordWithKind(records, "status")
	if status["mode"] != "host-required" || status["mirror"] != "clean" {
		t.Fatalf("status record = %#v", status)
	}
	device := status["device"].(map[string]any)
	if device["compiler"] != "host-required" || device["apply_bytes"] != float64(92) {
		t.Fatalf("device status = %#v", device)
	}
	send := recordWithKind(records, "send")
	if send["action"] != "apply" || send["mirror"] != "pending" ||
		send["source"] != "time is 200" || send["line"] != "apply 0102" {
		t.Fatalf("send record = %#v", send)
	}
	response := recordWithKind(records, "response")
	if response["mirror"] != "clean" || response["mirror_action"] != "commit" ||
		response["ok"] != true || response["status"] != "ok" {
		t.Fatalf("response record = %#v", response)
	}
	if comp.commits != 1 || comp.drops != 0 {
		t.Fatalf("commits=%d drops=%d, want commits=1 drops=0", comp.commits, comp.drops)
	}
	sessionEnd := recordWithKind(records, "session_end")
	if sessionEnd["mirror"] != "clean" {
		t.Fatalf("session_end record = %#v", sessionEnd)
	}
}

func TestRecordsLateSignalAfterAcceptedUpdateCommits(t *testing.T) {
	tests := []struct {
		name   string
		action compileAction
		line   string
		source string
	}{
		{name: "apply", action: actionApply, line: "apply 0102", source: "time is 200\n"},
		{name: "clear", action: actionClear, line: "clear", source: "clear\n"},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			tracker := &interruptTracker{}
			comp := &fakeCompiler{
				target: targetProfile("1234abcd"),
				results: []compileResult{
					{action: test.action, line: test.line},
				},
			}
			var dev *fakeDevice
			dev = &fakeDevice{
				responses: []string{
					statusResponse("host-required"),
					"ok\n",
				},
				afterPrompt: func(line string) {
					if line == test.line && tracker.requestAndShouldForward() {
						_ = dev.sendInterrupt()
					}
				},
			}
			var out strings.Builder

			err := runRecordsTestSession(t, strings.NewReader(test.source), &out, comp, dev, time.Second, false, tracker)
			if err != nil {
				t.Fatal(err)
			}

			records := decodeRecords(t, out.String())
			if got, want := recordKinds(records), "session_start,status,send,response,session_end"; got != want {
				t.Fatalf("record kinds %q, want %q", got, want)
			}
			response := recordWithKind(records, "response")
			if response["mirror"] != "clean" || response["mirror_action"] != "commit" ||
				response["ok"] != true || response["status"] != "ok" {
				t.Fatalf("response record = %#v", response)
			}
			if comp.commits != 1 || comp.drops != 0 {
				t.Fatalf("commits=%d drops=%d, want commits=1 drops=0", comp.commits, comp.drops)
			}
			if dev.interrupts != 0 {
				t.Fatalf("interrupts=%d, want 0", dev.interrupts)
			}
		})
	}
}

func TestRecordsGroupMultilineTopForm(t *testing.T) {
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionApply, line: "apply 0102"},
		},
	}
	dev := &fakeDevice{responses: []string{statusResponse("host-required"), "ok\n"}}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("boot is fn [\n  one\n]\n"), &out, comp, dev, time.Second, false, &interruptTracker{})
	if err != nil {
		t.Fatal(err)
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,send,response,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	send := recordWithKind(records, "send")
	if send["source"] != "boot is fn [ one ]" || send["line"] != "apply 0102" {
		t.Fatalf("send record = %#v", send)
	}
	if got, want := strings.Join(comp.lines, "\n"), "boot is fn [ one ]"; got != want {
		t.Fatalf("compiled %q, want %q", got, want)
	}
}

func TestRecordsRejectedApplyDropsMirror(t *testing.T) {
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionApply, line: "apply 0102"},
		},
	}
	dev := &fakeDevice{responses: []string{statusResponse("host-required"), "err 9\n"}}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("time is 200\n"), &out, comp, dev, time.Second, false, &interruptTracker{})
	if err != nil {
		t.Fatal(err)
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,send,response,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	response := recordWithKind(records, "response")
	if response["mirror"] != "clean" || response["mirror_action"] != "drop" ||
		response["ok"] != false || response["status"] != "err 9" {
		t.Fatalf("response record = %#v", response)
	}
	if comp.commits != 0 || comp.drops != 1 {
		t.Fatalf("commits=%d drops=%d, want commits=0 drops=1", comp.commits, comp.drops)
	}
}

func TestRecordsDeviceCompilerDirectSend(t *testing.T) {
	dev := &fakeDevice{responses: []string{statusResponse("device"), "ok\n"}}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("words\n"), &out, nil, dev, time.Second, false, &interruptTracker{})
	if err != nil {
		t.Fatal(err)
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,send,response,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	status := recordWithKind(records, "status")
	if status["mode"] != "device" || status["mirror"] != "none" {
		t.Fatalf("status record = %#v", status)
	}
	send := recordWithKind(records, "send")
	if send["action"] != "direct" || send["mirror"] != "none" || send["line"] != "words" {
		t.Fatalf("send record = %#v", send)
	}
}

func TestRecordsCompileErrorDoesNotSendDeviceLine(t *testing.T) {
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionError, line: "err 4 apply_bytes=92 required=120"},
		},
	}
	dev := &fakeDevice{responses: []string{statusResponse("host-required")}}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("too_big is fn [ words ]\n"), &out, comp, dev, time.Second, false, &interruptTracker{})
	if err != nil {
		t.Fatal(err)
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,compile_error,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	compileError := recordWithKind(records, "compile_error")
	if compileError["reason"] != "budget" || compileError["status"] != "err 4 apply_bytes=92 required=120" {
		t.Fatalf("compile_error record = %#v", compileError)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestRecordsUpdateTimeoutEmitsStale(t *testing.T) {
	tests := []struct {
		name   string
		action compileAction
		line   string
	}{
		{name: "apply", action: actionApply, line: "apply 0102"},
		{name: "clear", action: actionClear, line: "clear"},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			comp := &fakeCompiler{
				target: targetProfile("1234abcd"),
				results: []compileResult{
					{action: test.action, line: test.line},
				},
			}
			dev := &fakeDevice{
				responses: []string{
					statusResponse("host-required"),
					"",
				},
				responseErrs: []error{nil, errPromptTimeout},
				interruptResponses: []string{
					"err 10\n",
				},
			}
			var out strings.Builder

			err := runRecordsTestSession(t, strings.NewReader("blink is fn [ words ]\n"), &out, comp, dev, time.Second, false, &interruptTracker{})
			if err == nil {
				t.Fatal("runSerialRecords accepted stale update timeout")
			}

			records := decodeRecords(t, out.String())
			if got, want := recordKinds(records), "session_start,status,send,interrupt,session_error"; got != want {
				t.Fatalf("record kinds %q, want %q", got, want)
			}
			send := recordWithKind(records, "send")
			if send["action"] != recordAction(test.action) || send["mirror"] != "pending" {
				t.Fatalf("send record = %#v", send)
			}
			interrupt := recordWithKind(records, "interrupt")
			if interrupt["state"] != "stale" || interrupt["mirror"] != "stale" ||
				interrupt["settled"] != true || interrupt["status"] != "err 10" {
				t.Fatalf("interrupt record = %#v", interrupt)
			}
			sessionError := recordWithKind(records, "session_error")
			if sessionError["state"] != "stale" || sessionError["mirror"] != "stale" ||
				sessionError["code"] != "mirror_stale" {
				t.Fatalf("session_error record = %#v", sessionError)
			}
			if comp.commits != 0 || comp.drops != 1 {
				t.Fatalf("commits=%d drops=%d, want commits=0 drops=1", comp.commits, comp.drops)
			}
		})
	}
}

func TestRecordsForegroundTimeoutInterruptsAndContinues(t *testing.T) {
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionSend, line: "run dead"},
			{action: actionSend, line: "run beef"},
		},
	}
	dev := &fakeDevice{
		responses: []string{
			statusResponse("host-required"),
			"err ",
			"ok\n",
		},
		responseErrs: []error{nil, errPromptTimeout, nil},
		interruptResponses: []string{
			"10\n",
		},
	}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("forever [ 1 ]\nblink:\n"), &out, comp, dev, time.Second, false, &interruptTracker{})
	if err != nil {
		t.Fatal(err)
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,send,interrupt,send,response,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	interrupt := recordWithKind(records, "interrupt")
	if interrupt["state"] != "idle" || interrupt["mirror"] != "clean" ||
		interrupt["settled"] != true || interrupt["status"] != "err 10" {
		t.Fatalf("interrupt record = %#v", interrupt)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nrun dead\nrun beef"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestRecordsSignalInterruptUsesTracker(t *testing.T) {
	tracker := &interruptTracker{}
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionSend, line: "run dead"},
		},
	}
	dev := &fakeDevice{
		responses: []string{
			statusResponse("host-required"),
			"err 10\n",
		},
		onSend: func(line string) {
			if line == "run dead" {
				tracker.request()
			}
		},
	}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("forever [ 1 ]\n"), &out, comp, dev, time.Second, false, tracker)
	if err != nil {
		t.Fatal(err)
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,send,interrupt,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	interrupt := recordWithKind(records, "interrupt")
	if interrupt["state"] != "idle" || interrupt["mirror"] != "clean" ||
		interrupt["settled"] != true || interrupt["status"] != "err 10" {
		t.Fatalf("interrupt record = %#v", interrupt)
	}
}

func TestRecordsSignalInterruptDuringApplyStalesMirror(t *testing.T) {
	tracker := &interruptTracker{}
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionApply, line: "apply 0102"},
		},
	}
	dev := &fakeDevice{
		responses: []string{
			statusResponse("host-required"),
			"err 10\n",
		},
		onSend: func(line string) {
			if line == "apply 0102" {
				tracker.request()
			}
		},
	}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("time is 200\n"), &out, comp, dev, time.Second, false, tracker)
	if err == nil {
		t.Fatal("runRecordsTestSession accepted interrupted apply")
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,send,interrupt,session_error"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	interrupt := recordWithKind(records, "interrupt")
	if interrupt["state"] != "stale" || interrupt["mirror"] != "stale" ||
		interrupt["settled"] != true || interrupt["status"] != "err 10" {
		t.Fatalf("interrupt record = %#v", interrupt)
	}
	sessionError := recordWithKind(records, "session_error")
	if sessionError["state"] != "stale" || sessionError["mirror"] != "stale" ||
		sessionError["code"] != "mirror_stale" {
		t.Fatalf("session_error record = %#v", sessionError)
	}
	if comp.commits != 0 || comp.drops != 1 {
		t.Fatalf("commits=%d drops=%d, want commits=0 drops=1", comp.commits, comp.drops)
	}
}

func TestRecordsUnsettledSignalInterruptFails(t *testing.T) {
	tracker := &interruptTracker{}
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionSend, line: "run dead"},
		},
	}
	dev := &fakeDevice{
		responses: []string{
			statusResponse("host-required"),
			"boot banner\n",
		},
		onSend: func(line string) {
			if line == "run dead" {
				tracker.request()
			}
		},
	}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("forever [ 1 ]\n"), &out, comp, dev, time.Second, false, tracker)
	if err == nil {
		t.Fatal("runRecordsTestSession accepted unsettled interrupt")
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,send,interrupt,session_error"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	interrupt := recordWithKind(records, "interrupt")
	if interrupt["state"] != "error" || interrupt["mirror"] != "clean" ||
		interrupt["settled"] != false || interrupt["code"] != "interrupt_failed" {
		t.Fatalf("interrupt record = %#v", interrupt)
	}
	sessionError := recordWithKind(records, "session_error")
	if sessionError["state"] != "error" || sessionError["mirror"] != "clean" ||
		sessionError["code"] != "interrupt_failed" {
		t.Fatalf("session_error record = %#v", sessionError)
	}
}
