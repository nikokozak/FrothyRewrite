package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

type fakeDevice struct {
	responses          []string
	responseErrs       []error
	interruptResponses []string
	interruptReadErrs  []error
	interruptErrs      []error
	syncErrs           []error
	sent               []string
	interrupts         int
	syncs              int
	onSend             func(line string)
	afterPrompt        func(line string)
}

func (d *fakeDevice) syncPrompt(timeout time.Duration) error {
	_ = timeout
	d.syncs += 1
	if len(d.syncErrs) == 0 {
		return nil
	}
	err := d.syncErrs[0]
	d.syncErrs = d.syncErrs[1:]
	return err
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

func TestWireRequestEnvelopesMultilineAndReservedSource(t *testing.T) {
	tests := []struct {
		name   string
		source string
		want   string
	}{
		{name: "single line", source: `print: "a\\nb"`, want: `print: "a\\nb"`},
		{name: "ok status", source: "ok", want: "source-form ok"},
		{name: "legacy error", source: "err 8", want: "source-form err 8"},
		{name: "canonical error", source: "error: fake (3)", want: "source-form error: fake (3)"},
		{name: "canonical notice", source: "notice: fake (13)", want: "source-form notice: fake (13)"},
		{name: "noncanonical error", source: "error: fake(3)", want: "error: fake(3)"},
		{name: "noncanonical notice", source: "notice: fake(13)", want: "notice: fake(13)"},
		{name: "async prefix", source: "! forged", want: "source-form ! forged"},
		{name: "prompt prefix", source: "> err 99", want: "source-form > err 99"},
		{name: "transport tab", source: "\tok", want: "source-form \tok"},
		{name: "transport unicode", source: "éok", want: "source-form éok"},
		{
			name:   "source form prefix",
			source: `source-form value\nnext`,
			want:   `source-form source-form value\\nnext`,
		},
		{
			name:   "newlines and backslashes",
			source: "to f [\r\n  print: \"a\\nb\"\r]",
			want:   `source-form to f [\n  print: "a\\nb"\n]`,
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := wireRequest(test.source); got != test.want {
				t.Fatalf("wireRequest(%q) = %q, want %q", test.source, got, test.want)
			}
		})
	}
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
	if got := responseStatus(response); got != "ok" || !responseOK(response) {
		t.Fatalf("initial-prompt response status = %q, ok = %v", got,
			responseOK(response))
	}
}

func TestSerialReadUntilPromptAcceptsLegacyErrorStatus(t *testing.T) {
	dev := serialDeviceWithReadBytes("err 8\n> ")

	response, err := dev.readUntilPrompt(time.Second, true, nil)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := responseStatus(response), "err 8"; got != want {
		t.Fatalf("responseStatus(%q) = %q, want %q", response, got, want)
	}
}

func TestSerialReadUntilPromptAcceptsRichErrorStatus(t *testing.T) {
	dev := serialDeviceWithReadBytes("error: not found (7)\nname: missing\nmissing[0]\n^^^^^^^\n> ")

	response, err := dev.readUntilPrompt(time.Second, true, nil)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := response, "error: not found (7)\nname: missing\nmissing[0]\n^^^^^^^\n"; got != want {
		t.Fatalf("response %q, want %q", got, want)
	}
	if got, want := responseStatus(response), "error: not found (7)"; got != want {
		t.Fatalf("responseStatus(%q) = %q, want %q", response, got, want)
	}
	if responseOK(response) {
		t.Fatalf("responseOK(%q) = true, want false", response)
	}
}

func TestSerialReadUntilPromptIgnoresPromptTextInsideDiagnosticLine(t *testing.T) {
	want := "error: not found (7)\nname: nosuchword\n" +
		"if 1 > 2 [ nosuchword ]\n           ^^^^^^^^^^\n"
	dev := serialDeviceWithReadBytes(want + "> ")

	response, err := dev.readUntilPrompt(time.Second, true, nil)
	if err != nil {
		t.Fatal(err)
	}
	if response != want {
		t.Fatalf("response %q, want %q", response, want)
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

func TestResponseStatusUsesFirstTerminalLine(t *testing.T) {
	tests := []struct {
		name     string
		response string
		want     string
	}{
		{name: "plain ok", response: "ok\n", want: "ok"},
		{name: "echoed apply ok", response: "apply 1234\r\nok\r\n", want: "ok"},
		{name: "notice then ok", response: "notice: not saved (13)\ndetail: still live\nok\n", want: "ok"},
		{name: "plain error", response: "error: unsupported (9)\n", want: "error: unsupported (9)"},
		{name: "echoed apply error", response: "apply 1234\r\nerror: unsupported (9)\r\n", want: "error: unsupported (9)"},
		{name: "legacy error", response: "err 8\n", want: "err 8"},
		{name: "printed error headline is terminal", response: "error: boom (1)\nok\n", want: "error: boom (1)"},
		{name: "later error-shaped detail is opaque", response: "error: not found (7)\nerror: body text (25)\n", want: "error: not found (7)"},
		{name: "padded echo is not status", response: "ok \r\nerror: not found (7)\r\n", want: "error: not found (7)"},
		{name: "padded ok alone is malformed", response: "ok \n", want: "ok "},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := responseStatus(test.response); got != test.want {
				t.Fatalf("responseStatus(%q) = %q, want %q", test.response, got,
					test.want)
			}
			if got, want := responseOK(test.response), test.want == "ok"; got != want {
				t.Fatalf("responseOK(%q) = %v, want %v", test.response, got, want)
			}
		})
	}
}

func TestResponseNoticeKeepsHeadlineAndDetails(t *testing.T) {
	response := "save\r\nnotice: not saved (13)\r\n" +
		"detail: first reason\r\n! tick\r\ndetail: second reason\r\nok\r\n"
	if got, want := responseNoticeStatus(response), "notice: not saved (13)"; got != want {
		t.Fatalf("responseNoticeStatus = %q, want %q", got, want)
	}
	if got, want := responseNoticeText(response),
		"notice: not saved (13)\n"+
			"detail: first reason\n"+
			"detail: second reason\n"; got != want {
		t.Fatalf("responseNoticeText = %q, want %q", got, want)
	}

	errorResponse := "notice: quoted source (13)\nerror: invalid (8)\n"
	if got := responseNoticeStatus(errorResponse); got != "" {
		t.Fatalf("failed response reported notice %q", got)
	}

	paddedNotice := "notice: fake (13) \nok\n"
	if got := responseNoticeStatus(paddedNotice); got != "" {
		t.Fatalf("padded line fabricated notice %q", got)
	}

	paddedDetail := "notice: not saved (13)\n detail: not a detail\n" +
		"detail: real reason\nok\n"
	if got, want := responseNoticeText(paddedDetail),
		"notice: not saved (13)\ndetail: real reason\n"; got != want {
		t.Fatalf("responseNoticeText = %q, want %q", got, want)
	}
}

func TestResponseTerminalStatusRequiresWholeLine(t *testing.T) {
	tests := []struct {
		response string
		want     bool
	}{
		{response: "ok\n", want: true},
		{response: "err 8\n", want: true},
		{response: "error: invalid (8)\n", want: true},
		{response: "> ok\n", want: true},
		{response: "not ok\n", want: false},
		{response: "ok \n", want: false},
	}
	for _, test := range tests {
		if got := responseHasTerminalStatus(test.response); got != test.want {
			t.Errorf("responseHasTerminalStatus(%q) = %v, want %v",
				test.response, got, test.want)
		}
	}
	if promptComplete("error: not found (7)\nif 1 > ", true) {
		t.Fatal("mid-line prompt text completed the response frame")
	}
}

func TestReservedSourceEnvelopeCannotBecomeResponseStatus(t *testing.T) {
	source := "error: pasted (3)"
	response := "! tick\r\n" + wireRequest(source) + "\r\n" +
		"error: not found (7)\r\nname: error\r\nerror: body text (25)\r\n^\r\n"
	if got, want := responseStatus(response), "error: not found (7)"; got != want {
		t.Fatalf("responseStatus = %q, want %q", got, want)
	}
	if responseOK(response) {
		t.Fatal("reserved error-shaped source reported ok")
	}

	noticeSource := "notice: pasted (13)"
	noticeEcho := "! tick\r\n" + wireRequest(noticeSource) + "\r\nok\r\n"
	if got := responseNoticeStatus(noticeEcho); got != "" {
		t.Fatalf("echoed source fabricated notice %q", got)
	}
	if got := responseNoticeText(noticeEcho); got != "" {
		t.Fatalf("echoed source fabricated notice text %q", got)
	}
}

func statusResponse(compiler string) string {
	return "status\r\nfrothy status v1 profile=test profile_hash=1234abcd compiler=" +
		compiler + " names=device storage=volatile interrupt=cooperative word_size=16 int_min=-16384 int_max=16383 apply_bytes=92\r\nok\r\n"
}

func statusResponse32(compiler string) string {
	return "status\r\nfrothy status v1 release=vTEST profile=test32 profile_hash=bead1234 compiler=" +
		compiler + " names=device storage=volatile interrupt=cooperative word_size=32 int_min=-1073741824 int_max=1073741823 apply_bytes=128\r\nok\r\n"
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

func TestFrothyHelpGroupsCommands(t *testing.T) {
	var out bytes.Buffer
	printFrothyUsage(&out, availableVerbs())
	last := -1
	for _, group := range []string{"Start", "Work", "Project", "Recover", "Editor plumbing"} {
		heading := group + ":"
		if strings.Count(out.String(), heading) != 1 {
			t.Fatalf("help must contain one %q heading:\n%s", heading, out.String())
		}
		at := strings.Index(out.String(), heading)
		if at <= last {
			t.Fatalf("help group %q is out of order:\n%s", group, out.String())
		}
		last = at
	}
	for _, want := range []string{
		"Start:\n  menu",
		"Work:\n  send",
		"Project:\n  flash",
		"Recover:\n  wipe",
		"Editor plumbing:\n  session",
	} {
		if !strings.Contains(out.String(), want) {
			t.Fatalf("help missing %q:\n%s", want, out.String())
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
	code := runFrothyCommand([]string{"/usr/local/bin/frothy", "session", "--records"}, &stdout, &stderr, verbs)
	if code != 0 {
		t.Fatalf("exit code %d, want 0", code)
	}
	if !ran {
		t.Fatal("verb run was not called")
	}
	if got, want := strings.Join(os.Args, "\n"), "/usr/local/bin/frothy session\n--records"; got != want {
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

func TestFrothySendRejectsRetiredCompilerFlags(t *testing.T) {
	for _, retired := range []string{"--dry-run", "--compiler", "--host-compile"} {
		t.Run(retired, func(t *testing.T) {
			var stdout, stderr bytes.Buffer
			code := runSendCommand([]string{retired}, &stdout, &stderr)
			if code != 2 {
				t.Fatalf("exit code %d, want 2; stderr=%q", code, stderr.String())
			}
			if !strings.Contains(stderr.String(), "flag provided but not defined") {
				t.Fatalf("stderr does not reject %s: %q", retired, stderr.String())
			}
		})
	}
}

func TestFrothySendErrorsOnMissingFile(t *testing.T) {
	var stdout, stderr bytes.Buffer
	code := runSendCommand([]string{"--port", "/dev/cu.test", "/nonexistent/missing-source.frothy"}, &stdout, &stderr)
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

func TestFrothySendRejectsDirectory(t *testing.T) {
	var stdout, stderr bytes.Buffer
	code := runSendCommand([]string{"--port", "/dev/cu.test", t.TempDir()}, &stdout, &stderr)
	if code != 1 {
		t.Fatalf("exit code %d, want 1", code)
	}
	if !strings.Contains(stderr.String(), "is not a file") {
		t.Fatalf("stderr missing file-kind error: %q", stderr.String())
	}
}

func TestFrothySendErrorsWhenPortMissing(t *testing.T) {
	var stdout, stderr bytes.Buffer
	code := runSendCommand([]string{"some.frothy"}, &stdout, &stderr)
	if code == 0 {
		t.Fatal("exit code 0, want non-zero")
	}
	if !strings.Contains(stderr.String(), "--port is required") {
		t.Fatalf("stderr missing --port required message: %q", stderr.String())
	}
}

func TestPickPortChoosesSessionPortWhenUnset(t *testing.T) {
	list := func() ([]string, error) {
		return []string{"/dev/cu.usbserial-0001"}, nil
	}
	got, err := pickSessionPort("", list, nil)
	if err != nil {
		t.Fatal(err)
	}
	if got != "/dev/cu.usbserial-0001" {
		t.Fatalf("pickSessionPort() = %q, want discovered port", got)
	}
}

func TestPickPortOverrideSkipsDiscovery(t *testing.T) {
	list := func() ([]string, error) {
		t.Fatal("lister must not run when --port is set")
		return nil, nil
	}
	got, err := pickPort("/dev/cu.usbserial-OVERRIDE", list)
	if err != nil {
		t.Fatal(err)
	}
	if got != "/dev/cu.usbserial-OVERRIDE" {
		t.Fatalf("got %q, want override path", got)
	}
}

func TestPickPortChoosesTheSingleSerialMatch(t *testing.T) {
	list := func() ([]string, error) {
		return []string{"/dev/null", "/dev/cu.usbmodem101", "/dev/random"}, nil
	}
	got, err := pickPort("", list)
	if err != nil {
		t.Fatal(err)
	}
	if got != "/dev/cu.usbmodem101" {
		t.Fatalf("got %q, want /dev/cu.usbmodem101", got)
	}
}

func TestPickPortErrorsWhenNoSerialPortFound(t *testing.T) {
	list := func() ([]string, error) {
		return []string{"/dev/null", "/dev/random"}, nil
	}
	_, err := pickPort("", list)
	if err == nil {
		t.Fatal("err nil, want error")
	}
	if got, want := err.Error(), "no serial port found; pass --port"; got != want {
		t.Fatalf("err %q, want %q", got, want)
	}
	var selection *portSelectionError
	if !errors.As(err, &selection) || selection.code != recordErrorNoPorts || len(selection.candidates) != 0 {
		t.Fatalf("selection error = %#v", selection)
	}
}

func TestPickPortErrorsAndListsCandidatesWhenAmbiguous(t *testing.T) {
	list := func() ([]string, error) {
		return []string{"/dev/cu.usbmodem101", "/dev/cu.usbserial-0001", "/dev/null"}, nil
	}
	_, err := pickPort("", list)
	if err == nil {
		t.Fatal("err nil, want error")
	}
	if got, want := err.Error(), "multiple serial ports found: /dev/cu.usbmodem101, /dev/cu.usbserial-0001; pass --port to choose"; got != want {
		t.Fatalf("err %q, want %q", got, want)
	}
	var selection *portSelectionError
	if !errors.As(err, &selection) || selection.code != recordErrorMultiplePorts ||
		strings.Join(selection.candidates, ",") != "/dev/cu.usbmodem101,/dev/cu.usbserial-0001" {
		t.Fatalf("selection error = %#v", selection)
	}
}

func TestFrothyFlashBuildsMakeArgvWithDiscoveredPort(t *testing.T) {
	root := makeFlashTestRoot(t)
	var gotName string
	var gotArgs []string
	runner := func(name string, args []string) error {
		gotName = name
		gotArgs = args
		return nil
	}
	list := func() ([]string, error) {
		return []string{"/dev/null", "/dev/cu.usbserial-0001"}, nil
	}

	var stderr bytes.Buffer
	code := runFlashCommand([]string{"esp32_devkit_v1"}, root, "", io.Discard, &stderr, list, runner)
	if code != 0 {
		t.Fatalf("exit code %d, want 0; stderr=%q", code, stderr.String())
	}
	if gotName != "make" {
		t.Fatalf("ran %q, want make", gotName)
	}
	want := []string{"-C", root, "flash", "BOARD=esp32_devkit_v1", "BOARD_PORT=/dev/cu.usbserial-0001"}
	if strings.Join(gotArgs, " ") != strings.Join(want, " ") {
		t.Fatalf("argv=%q, want %q", gotArgs, want)
	}
}

func TestFrothyFlashPortOverrideSkipsDiscovery(t *testing.T) {
	root := makeFlashTestRoot(t)
	var gotArgs []string
	runner := func(_ string, args []string) error {
		gotArgs = args
		return nil
	}
	list := func() ([]string, error) {
		t.Fatal("lister must not run when --port is set")
		return nil, nil
	}

	var stderr bytes.Buffer
	code := runFlashCommand([]string{"--port", "/dev/cu.usbmodem999", "esp32_devkit_v1"}, root, "", io.Discard, &stderr, list, runner)
	if code != 0 {
		t.Fatalf("exit code %d, want 0; stderr=%q", code, stderr.String())
	}
	if got, want := strings.Join(gotArgs, " "), "-C "+root+" flash BOARD=esp32_devkit_v1 BOARD_PORT=/dev/cu.usbmodem999"; got != want {
		t.Fatalf("argv=%q, want override port", got)
	}
}

func TestFrothyFlashUsesPackagedFirmwareWithoutSourceCheckout(t *testing.T) {
	prefix := t.TempDir()
	executable := filepath.Join(prefix, "bin", "frothy")
	firmwareRoot := filepath.Join(prefix, "share", "frothy", "firmware")
	writeFile(t, executable, "binary")
	writeFile(t, filepath.Join(firmwareRoot, "boot.bin"), "boot")
	writeFile(t, filepath.Join(firmwareRoot, "app.bin"), "app")
	writeFile(t, filepath.Join(firmwareRoot, "manifest.json"), `[
  {"board":"esp32_devkit_v1","chip":"esp32","segments":[
    {"address":4096,"file":"boot.bin"},
    {"address":65536,"file":"app.bin"}
  ]}
]`)
	firmwareRoot = canonicalPath(firmwareRoot)
	if got := packagedFirmwareRoot(executable); got != firmwareRoot {
		t.Fatalf("firmware root = %q, want %q", got, firmwareRoot)
	}

	var gotName string
	var gotArgs []string
	runner := func(name string, args []string) error {
		gotName = name
		gotArgs = args
		return nil
	}
	list := func() ([]string, error) {
		t.Fatal("lister must not run when --port is set")
		return nil, nil
	}

	var stderr bytes.Buffer
	code := runFlashCommand([]string{"--port", "/dev/cu.usbserial-0001", "esp32_devkit_v1"},
		"", firmwareRoot, io.Discard, &stderr, list, runner)
	if code != 0 {
		t.Fatalf("exit code %d, want 0; stderr=%q", code, stderr.String())
	}
	if gotName != "esptool" {
		t.Fatalf("ran %q, want esptool", gotName)
	}
	want := "--chip esp32 --port /dev/cu.usbserial-0001 --baud 460800 " +
		"--before default-reset --after hard-reset write-flash --flash-mode keep " +
		"--flash-freq keep --flash-size keep 0x1000 " + filepath.Join(firmwareRoot, "boot.bin") +
		" 0x10000 " + filepath.Join(firmwareRoot, "app.bin")
	if got := strings.Join(gotArgs, " "); got != want {
		t.Fatalf("argv=%q, want %q", got, want)
	}
}

func TestFrothyFlashSuggestsStopWhenPortLooksBusy(t *testing.T) {
	root := makeFlashTestRoot(t)
	runner := func(string, []string) error {
		return commandOutputError{err: errors.New("exit status 2"), output: "serial port is busy"}
	}
	list := func() ([]string, error) {
		return []string{"/dev/cu.usbserial-0001"}, nil
	}

	var stderr bytes.Buffer
	code := runFlashCommand([]string{"esp32_devkit_v1"}, root, "", io.Discard, &stderr, list, runner)
	if code != 1 {
		t.Fatalf("exit code %d, want 1", code)
	}
	if !strings.Contains(stderr.String(), "frothy stop") {
		t.Fatalf("stderr missing frothy stop hint: %q", stderr.String())
	}
}

func TestFrothyFlashErrorsOnUnknownBoardBeforeInvokingMake(t *testing.T) {
	root := makeFlashTestRoot(t)
	runner := func(string, []string) error {
		t.Fatal("runner must not run when the board is unknown")
		return nil
	}
	list := func() ([]string, error) {
		t.Fatal("lister must not run when the board is unknown")
		return nil, nil
	}
	var stderr bytes.Buffer
	code := runFlashCommand([]string{"made_up_board"}, root, "", io.Discard, &stderr, list, runner)
	if code == 0 {
		t.Fatal("exit code 0, want non-zero")
	}
	if !strings.Contains(stderr.String(), `unknown board "made_up_board"`) {
		t.Fatalf("stderr missing unknown-board message: %q", stderr.String())
	}
}

func TestFrothyFlashErrorsWhenNoSerialPortFound(t *testing.T) {
	root := makeFlashTestRoot(t)
	runner := func(string, []string) error {
		t.Fatal("runner must not run when port discovery fails")
		return nil
	}
	list := func() ([]string, error) {
		return []string{"/dev/null", "/dev/random"}, nil
	}

	var stderr bytes.Buffer
	code := runFlashCommand([]string{"esp32_devkit_v1"}, root, "", io.Discard, &stderr, list, runner)
	if code == 0 {
		t.Fatal("exit code 0, want non-zero")
	}
	if !strings.Contains(stderr.String(), "no serial port") || !strings.Contains(stderr.String(), "--port") {
		t.Fatalf("stderr missing discovery guidance: %q", stderr.String())
	}
}

func TestFrothyFlashErrorsWhenMultipleSerialPortsFound(t *testing.T) {
	root := makeFlashTestRoot(t)
	runner := func(string, []string) error {
		t.Fatal("runner must not run when port discovery is ambiguous")
		return nil
	}
	list := func() ([]string, error) {
		return []string{"/dev/cu.usbserial-0001", "/dev/cu.usbmodem101"}, nil
	}

	var stderr bytes.Buffer
	code := runFlashCommand([]string{"esp32_devkit_v1"}, root, "", io.Discard, &stderr, list, runner)
	if code == 0 {
		t.Fatal("exit code 0, want non-zero")
	}
	msg := stderr.String()
	if !strings.Contains(msg, "/dev/cu.usbserial-0001") || !strings.Contains(msg, "/dev/cu.usbmodem101") {
		t.Fatalf("stderr does not list both candidates: %q", msg)
	}
	if !strings.Contains(msg, "--port") {
		t.Fatalf("stderr missing --port guidance: %q", msg)
	}
}

func TestFrothyFlashAndWipeRejectMissingInstallBeforePortSelection(t *testing.T) {
	list := func() ([]string, error) {
		t.Fatal("lister must not run without a source root")
		return nil, nil
	}
	runner := func(string, []string) error {
		t.Fatal("runner must not run without a source root")
		return nil
	}
	for _, test := range []struct {
		name string
		want string
		run  func(io.Writer) int
	}{
		{name: "flash", want: "no packaged firmware is installed", run: func(stderr io.Writer) int {
			return runFlashCommand([]string{"esp32_devkit_v1"}, "", "", io.Discard, stderr, list, runner)
		}},
		{name: "wipe", want: frothySourceRootEnv, run: func(stderr io.Writer) int {
			return runWipeCommand([]string{"--force", "esp32_devkit_v1"}, "", io.Discard, stderr, list, runner)
		}},
	} {
		t.Run(test.name, func(t *testing.T) {
			var stderr bytes.Buffer
			if code := test.run(&stderr); code != 2 {
				t.Fatalf("exit code = %d, want 2", code)
			}
			if !strings.Contains(stderr.String(), test.want) {
				t.Fatalf("stderr = %q, want %q guidance", stderr.String(), test.want)
			}
		})
	}
}

func TestFrothyWipeCommand(t *testing.T) {
	cases := []struct {
		name       string
		args       []string
		wantExit   int
		wantStderr []string
		wantArgv   string
		wantLister bool
	}{
		{
			name:       "missing board name",
			args:       []string{"--force"},
			wantExit:   2,
			wantStderr: []string{"wipe: expected exactly one board name"},
		},
		{
			name:       "unsupported board",
			args:       []string{"--force", "made_up_board"},
			wantExit:   2,
			wantStderr: []string{`wipe: unsupported board "made_up_board"`},
		},
		{
			name:     "missing --force",
			args:     []string{"--port", "/dev/cu.usbserial-0001", "esp32_devkit_v1"},
			wantExit: 2,
			wantStderr: []string{
				"wipe: refusing to erase persisted device state without --force",
				"frothy wipe --force esp32_devkit_v1 --port /dev/cu.usbserial-0001",
			},
		},
		{
			name:       "happy path discovered port",
			args:       []string{"--force", "esp32_devkit_v1"},
			wantExit:   0,
			wantArgv:   "wipe-persist BOARD=esp32_devkit_v1 BOARD_PORT=/dev/cu.usbserial-0001",
			wantLister: true,
		},
		{
			name:     "happy path explicit port",
			args:     []string{"--force", "--port", "/dev/cu.usbmodem999", "esp32_devkit_v1"},
			wantExit: 0,
			wantArgv: "wipe-persist BOARD=esp32_devkit_v1 BOARD_PORT=/dev/cu.usbmodem999",
		},
		{
			name:     "xiao happy path explicit port",
			args:     []string{"--force", "--port", "/dev/cu.usbmodem999", "seeed_xiao_esp32s3"},
			wantExit: 0,
			wantArgv: "wipe-persist BOARD=seeed_xiao_esp32s3 BOARD_PORT=/dev/cu.usbmodem999",
		},
	}

	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			root := makeFlashTestRoot(t)
			var gotName string
			var gotArgs []string
			var runnerCalls int
			runner := func(name string, args []string) error {
				runnerCalls++
				gotName = name
				gotArgs = args
				return nil
			}

			var listerCalls int
			list := func() ([]string, error) {
				listerCalls++
				return []string{"/dev/null", "/dev/cu.usbserial-0001"}, nil
			}

			var stdout, stderr bytes.Buffer
			code := runWipeCommand(c.args, root, &stdout, &stderr, list, runner)
			if code != c.wantExit {
				t.Fatalf("exit code %d, want %d; stderr=%q", code, c.wantExit, stderr.String())
			}
			for _, want := range c.wantStderr {
				if !strings.Contains(stderr.String(), want) {
					t.Fatalf("stderr missing %q; got %q", want, stderr.String())
				}
			}
			if c.wantArgv == "" {
				if runnerCalls != 0 {
					t.Fatalf("runner called %d times, want 0", runnerCalls)
				}
			} else {
				if runnerCalls != 1 {
					t.Fatalf("runner called %d times, want 1", runnerCalls)
				}
				if gotName != "make" {
					t.Fatalf("ran %q, want make", gotName)
				}
				wantArgv := "-C " + root + " " + c.wantArgv
				if got := strings.Join(gotArgs, " "); got != wantArgv {
					t.Fatalf("argv=%q, want %q", got, wantArgv)
				}
				if stderr.Len() != 0 {
					t.Fatalf("stderr must be empty on success; got %q", stderr.String())
				}
			}
			if c.wantLister {
				if listerCalls == 0 {
					t.Fatal("lister must run when --port is not set")
				}
			} else {
				if listerCalls != 0 {
					t.Fatalf("lister called %d times, want 0", listerCalls)
				}
			}
		})
	}
}

func TestWipeRecoveryHintRequiresExplicitBoard(t *testing.T) {
	got := wipeRecoveryHint("/dev/cu.usbmodem101")
	for _, want := range []string{"choose the board explicitly", "wipe --force BOARD", "/dev/cu.usbmodem101"} {
		if !strings.Contains(got, want) {
			t.Fatalf("hint %q missing %q", got, want)
		}
	}
	if strings.Contains(got, "esp32_devkit_v1") {
		t.Fatalf("generic recovery hint guesses a board: %q", got)
	}
}

func makeFlashTestRoot(t *testing.T) string {
	t.Helper()
	root := makeSourceRoot(t)
	writeTestBoard(t, filepath.Join(root, "boards"), "esp32_devkit_v1", `{"target":"esp-idf"}`)
	writeTestBoard(t, filepath.Join(root, "boards"), "seeed_xiao_esp32s3", `{"target":"esp-idf"}`)
	return root
}

func TestFrothyDoctorExitsZeroWhenAllChecksPass(t *testing.T) {
	checks := []doctorCheck{
		{name: "make", run: func() (bool, string) { return true, "/usr/bin/make" }},
		{name: "serial", run: func() (bool, string) { return true, "/dev/cu.usbserial-0001" }},
		{name: "device", run: func() (bool, string) { return true, "/dev/cu.usbserial-0001" }},
	}

	var stdout, stderr bytes.Buffer
	code := runDoctorCommand(nil, &stdout, &stderr, checks)
	if code != 0 {
		t.Fatalf("exit code %d, want 0; stderr=%q", code, stderr.String())
	}
	for _, want := range []string{
		"ok    make: /usr/bin/make\n",
		"ok    serial: /dev/cu.usbserial-0001\n",
		"ok    device: /dev/cu.usbserial-0001\n",
	} {
		if !strings.Contains(stdout.String(), want) {
			t.Fatalf("stdout missing %q: %q", want, stdout.String())
		}
	}
	if strings.Contains(stdout.String(), "fail") {
		t.Fatalf("stdout has fail line when all checks pass: %q", stdout.String())
	}
}

func TestFrothyDoctorExitsNonZeroWhenAnyCheckFails(t *testing.T) {
	checks := []doctorCheck{
		{name: "make", run: func() (bool, string) { return false, "not on PATH; install build tools" }},
		{name: "serial", run: func() (bool, string) { return true, "/dev/cu.usbserial-0001" }},
	}

	var stdout, stderr bytes.Buffer
	code := runDoctorCommand(nil, &stdout, &stderr, checks)
	if code == 0 {
		t.Fatalf("exit code 0, want non-zero; stdout=%q", stdout.String())
	}
	if !strings.Contains(stdout.String(), "fail  make: not on PATH; install build tools\n") {
		t.Fatalf("stdout missing failed check: %q", stdout.String())
	}
	if !strings.Contains(stdout.String(), "ok    serial: ") {
		t.Fatalf("stdout missing passing checks: %q", stdout.String())
	}
}

func TestDoctorChecksIncludeFirmwareOnlyWithSource(t *testing.T) {
	for _, test := range []struct {
		name            string
		includeFirmware bool
		want            string
	}{
		{name: "installed", want: "serial,device"},
		{name: "source", includeFirmware: true, want: "make,serial,device,esp-idf-installed"},
	} {
		t.Run(test.name, func(t *testing.T) {
			checks := doctorChecks(test.includeFirmware)
			names := make([]string, 0, len(checks))
			for _, check := range checks {
				names = append(names, check.name)
			}
			if got := strings.Join(names, ","); got != test.want {
				t.Fatalf("checks = %q, want %q", got, test.want)
			}
		})
	}
}

func TestDoctorDeviceFailureReportsProbeErrorWithoutWipeAdvice(t *testing.T) {
	detail := formatDoctorUnresponsiveDevice("/dev/cu.usbserial-0001", errors.New("malformed status field: profilestatus"))
	if !strings.Contains(detail, "malformed status field: profilestatus") {
		t.Fatalf("detail missing probe error: %q", detail)
	}
	if strings.Contains(detail, "wipe") {
		t.Fatalf("detail should not suggest wipe for a failed probe: %q", detail)
	}
	if !strings.Contains(detail, "expected before first flash") {
		t.Fatalf("detail missing pre-flash context: %q", detail)
	}
}

func TestParseDeviceStatus(t *testing.T) {
	status, err := parseDeviceStatus(statusResponse("device"))
	if err != nil {
		t.Fatal(err)
	}
	if status.profile != "test" || status.profileHash != "1234abcd" ||
		status.compiler != compilerDevice || status.names != "device" ||
		status.storage != "volatile" || status.interrupt != "cooperative" ||
		status.wordSize != 16 || status.intMin != -16384 ||
		status.intMax != 16383 || status.applyBytes != 92 {
		t.Fatalf("status = %+v", status)
	}
}

func TestParseDeviceStatus32BitContract(t *testing.T) {
	status, err := parseDeviceStatus(statusResponse32("device"))
	if err != nil {
		t.Fatal(err)
	}
	if status.profile != "test32" || status.profileHash != "bead1234" ||
		status.compiler != compilerDevice || status.wordSize != 32 ||
		status.intMin != -1073741824 || status.intMax != 1073741823 ||
		status.applyBytes != 128 {
		t.Fatalf("status = %+v", status)
	}
}

func TestParseDeviceStatusRejectsUnsupportedCompiler(t *testing.T) {
	for _, mode := range []string{"unknown", "host-required", "host-optional"} {
		if _, err := parseDeviceStatus(statusResponse(mode)); err == nil {
			t.Fatalf("parseDeviceStatus accepted compiler mode %q", mode)
		}
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

func TestReadDeviceStatusRetriesPromptOnlyResponse(t *testing.T) {
	dev := &fakeDevice{responses: []string{"", statusResponse("device")}}

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
	if dev.syncs != 0 {
		t.Fatalf("syncs=%d, want 0", dev.syncs)
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
	if dev.syncs != 0 {
		t.Fatalf("syncs=%d, want 0", dev.syncs)
	}
}

func TestReadDeviceStatusRetriesMalformedFirstStatusLine(t *testing.T) {
	dev := &fakeDevice{responses: []string{
		"frothy status v1 profilestatus\nok\n",
		statusResponse("device"),
	}}

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
}

func TestReadDeviceStatusDoesNotRetrySemanticBadStatus(t *testing.T) {
	dev := &fakeDevice{responses: []string{statusResponse("bogus"), statusResponse("device")}}

	_, err := readDeviceStatus(dev, time.Second)
	if err == nil {
		t.Fatal("readDeviceStatus accepted bad compiler mode")
	}
	if got, want := strings.Join(dev.sent, "\n"), "status"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestReadDeviceStatusDoesNotRequirePassivePromptSync(t *testing.T) {
	dev := &fakeDevice{
		responses: []string{statusResponse("device")},
		syncErrs:  []error{errPromptTimeout},
	}

	status, err := readDeviceStatus(dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if status.compiler != compilerDevice {
		t.Fatalf("compiler = %s, want %s", status.compiler, compilerDevice)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	if dev.syncs != 0 {
		t.Fatalf("syncs=%d, want 0", dev.syncs)
	}
}

func TestReadDeviceStatusDoesNotRetryDeviceError(t *testing.T) {
	dev := &fakeDevice{responses: []string{"error: unsupported (9)\n", statusResponse("device")}}

	_, err := readDeviceStatus(dev, time.Second)
	if err == nil {
		t.Fatal("readDeviceStatus accepted device error")
	}
	if got, want := strings.Join(dev.sent, "\n"), "status"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	if dev.syncs != 0 {
		t.Fatalf("syncs=%d, want 0", dev.syncs)
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
	path := filepath.Join(t.TempDir(), "main.fr")
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

	read, ok, err := reader.next(&out, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !ok || read.source != "boot is fn [\n  one\n]" {
		t.Fatalf("first source=%q ok=%v, want grouped boot form", read.source, ok)
	}

	read, ok, err = reader.next(&out, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !ok || read.source != "words" {
		t.Fatalf("second source=%q ok=%v, want words", read.source, ok)
	}
	if got, want := out.String(), "frothy> .. .. frothy> "; got != want {
		t.Fatalf("prompts %q, want %q", got, want)
	}
}

func TestSourceFormReaderIgnoresCommentBrackets(t *testing.T) {
	reader := newSourceFormReader(strings.NewReader("-- header\nboot is fn [\n  -* ignored [ [\n  ] *-\n  one\n]\n"))
	var out strings.Builder

	read, ok, err := reader.next(&out, nil)
	if err != nil {
		t.Fatal(err)
	}
	if !ok || read.source != "boot is fn [\n  -* ignored [ [\n  ] *-\n  one\n]" {
		t.Fatalf("source=%q ok=%v, want grouped form with comments ignored", read.source, ok)
	}
	if got, want := out.String(), "frothy> frothy> .. .. .. .. "; got != want {
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

	read, ok, err := reader.next(&out, tracker)
	if err != nil {
		t.Fatal(err)
	}
	if !ok || read.source != "words" {
		t.Fatalf("source=%q ok=%v, want next line as top form after Ctrl-C", read.source, ok)
	}
	if got, want := out.String(), "frothy> .. "; got != want {
		t.Fatalf("prompts %q, want %q", got, want)
	}
}

func TestReadFileLinesMovesMultilineBootDefinitionLast(t *testing.T) {
	path := filepath.Join(t.TempDir(), "main.fr")
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
		"boot is fn [\n  blink:\n]",
	}
	if strings.Join(lines, "\n") != strings.Join(want, "\n") {
		t.Fatalf("readFileLines() = %#v, want %#v", lines, want)
	}
}

func TestReadFileLinesExpandsIncludesBeforeGrouping(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "main.fr")
	if err := os.WriteFile(filepath.Join(dir, "helper.fr"), []byte("helper is fn [ 1 ]\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("include \"helper.fr\"\nmain is fn [ helper: ]\n"), 0644); err != nil {
		t.Fatal(err)
	}

	lines, err := readFileLines(path)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{
		"helper is fn [ 1 ]",
		"main is fn [ helper: ]",
	}
	if strings.Join(lines, "\n") != strings.Join(want, "\n") {
		t.Fatalf("readFileLines() = %#v, want %#v", lines, want)
	}
}

func TestReadFileLinesDoesNotInterpretSourceBlockCommands(t *testing.T) {
	path := filepath.Join(t.TempDir(), "main.fr")
	if err := os.WriteFile(path, []byte(".source other.fr\nmain is fn [ 1 ]\n.end-source\n"), 0644); err != nil {
		t.Fatal(err)
	}

	lines, err := readFileLines(path)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{
		".source other.fr",
		"main is fn [ 1 ]",
		".end-source",
	}
	if strings.Join(lines, "\n") != strings.Join(want, "\n") {
		t.Fatalf("readFileLines() = %#v, want %#v", lines, want)
	}
}

func TestSerialSendsMultilineTopFormOnce(t *testing.T) {
	dev := &fakeDevice{responses: []string{statusResponse("device"), "ok\n"}}
	var out strings.Builder

	err := runSerial(strings.NewReader("boot is fn [\n  one\n]\n"), &out, dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nboot is fn [\n  one\n]"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	if got, want := out.String(), "frothy> .. .. ok\nfrothy> "; got != want {
		t.Fatalf("output %q, want %q", got, want)
	}
}

func TestSerialSourceBlockSendsBufferedFormsInSession(t *testing.T) {
	dev := &fakeDevice{responses: []string{statusResponse("device"), "ok\n", "ok\n"}}
	var out strings.Builder

	input := strings.NewReader(strings.Join([]string{
		".source playground.fr",
		"boot is fn [",
		"  blink:",
		"]",
		"led is $led_builtin",
		".end-source",
		"",
	}, "\n"))
	err := runSerial(input, &out, dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nled is $led_builtin\nboot is fn [\n  blink:\n]"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialSourceBlockUsesPathForIncludesWithoutSavingRoot(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "helper.fr"), []byte("helper is fn [ 1 ]\n"), 0644); err != nil {
		t.Fatal(err)
	}
	rootPath := filepath.Join(dir, "unsaved-main.fr")
	dev := &fakeDevice{responses: []string{statusResponse("device"), "ok\n", "ok\n"}}
	var out strings.Builder

	input := strings.NewReader(strings.Join([]string{
		".source " + rootPath,
		"include \"helper.fr\"",
		"main is fn [ helper: ]",
		".end-source",
		"",
	}, "\n"))
	err := runSerial(input, &out, dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nhelper is fn [ 1 ]\nmain is fn [ helper: ]"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialSourceBlockPathMayContainSpaces(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "project with spaces")
	if err := os.Mkdir(dir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "helper.fr"), []byte("helper is fn [ 1 ]\n"), 0644); err != nil {
		t.Fatal(err)
	}
	rootPath := filepath.Join(dir, "main.fr")
	dev := &fakeDevice{responses: []string{statusResponse("device"), "ok\n"}}
	var out strings.Builder

	input := strings.NewReader(strings.Join([]string{
		".source " + rootPath,
		"include \"helper.fr\"",
		".end-source",
		"",
	}, "\n"))
	err := runSerial(input, &out, dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nhelper is fn [ 1 ]"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialDeviceCompilerModeSendsSource(t *testing.T) {
	dev := &fakeDevice{responses: []string{statusResponse("device"), "ok\n"}}
	var out strings.Builder

	err := runSerial(strings.NewReader("led is $led_builtin\n"), &out, dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nled is $led_builtin"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialFileStopsOnDeviceError(t *testing.T) {
	dev := &fakeDevice{responses: []string{
		statusResponse("device"),
		"error: not found (7)\nname: ok\nok\n^^\n",
		"ok\n",
	}}
	var out strings.Builder

	err := runSerial(strings.NewReader("ok\n2 + 2\n"), &out, dev, time.Second)
	if err == nil || err.Error() != "device returned error: not found (7)" {
		t.Fatalf("error = %v, want device response error", err)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nok"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialFileContinuesAfterNotice(t *testing.T) {
	dev := &fakeDevice{responses: []string{
		statusResponse("device"),
		"notice: not saved (13)\ndetail: still live\nok\n",
		"4\nok\n",
	}}
	var out strings.Builder

	err := runSerial(strings.NewReader("save\n2 + 2\n"), &out, dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nsave\n2 + 2"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialRefusesUnknownStatusBeforeSendingSource(t *testing.T) {
	dev := &fakeDevice{responses: []string{statusResponse("unknown")}}
	var out strings.Builder

	err := runSerial(strings.NewReader("led is $led_builtin\n"), &out, dev, time.Second)
	if err == nil {
		t.Fatal("runSerial accepted unknown compiler status")
	}
	if got, want := strings.Join(dev.sent, "\n"), "status"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialRefusesRetiredHostCompilerMode(t *testing.T) {
	dev := &fakeDevice{responses: []string{statusResponse("host-required")}}
	var out strings.Builder

	err := runSerial(strings.NewReader("led is $led_builtin\n"), &out, dev, time.Second)
	if err == nil {
		t.Fatal("runSerial accepted retired host-required mode")
	}
	if got, want := strings.Join(dev.sent, "\n"), "status"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialForegroundTimeoutInterruptsAndContinues(t *testing.T) {
	dev := &fakeDevice{
		responses: []string{
			statusResponse("device"),
			"error: interrupted (",
			"ok\n",
		},
		responseErrs: []error{nil, errPromptTimeout, nil},
		interruptResponses: []string{
			"10)\n",
		},
	}
	var out strings.Builder

	err := runSerial(strings.NewReader("forever [ 1 ]\nblink:\n"), &out, dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if dev.interrupts != 1 {
		t.Fatalf("interrupts=%d, want 1", dev.interrupts)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nforever [ 1 ]\nblink:"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	if !strings.Contains(out.String(), "error: interrupted (10)\n") || !strings.Contains(out.String(), "ok\n") {
		t.Fatalf("output did not include recovery and next response: %q", out.String())
	}
}

func TestSerialForegroundTimeoutRequiresSettledRecovery(t *testing.T) {
	dev := &fakeDevice{
		responses: []string{
			statusResponse("device"),
			"",
		},
		responseErrs: []error{nil, errPromptTimeout},
		interruptResponses: []string{
			"boot banner\n",
		},
	}
	var out strings.Builder

	err := runSerial(strings.NewReader("forever [ 1 ]\nblink:\n"), &out, dev, time.Second)
	if err == nil {
		t.Fatal("runSerial accepted unsettled interrupt recovery")
	}
	if dev.interrupts != 1 {
		t.Fatalf("interrupts=%d, want 1", dev.interrupts)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nforever [ 1 ]"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialForegroundTimeoutFailsWhenInterruptDoesNotRecover(t *testing.T) {
	dev := &fakeDevice{
		responses: []string{
			statusResponse("device"),
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

	err := runSerial(strings.NewReader("forever [ 1 ]\nblink:\n"), &out, dev, time.Second)
	if err == nil {
		t.Fatal("runSerial accepted failed interrupt recovery")
	}
	if !strings.Contains(err.Error(), "interrupt recovery failed") {
		t.Fatalf("error %q does not explain failed recovery", err)
	}
	if dev.interrupts != 1 {
		t.Fatalf("interrupts=%d, want 1", dev.interrupts)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nforever [ 1 ]"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestSerialSignalInterruptUsesTracker(t *testing.T) {
	tracker := &interruptTracker{}
	dev := &fakeDevice{
		responses: []string{
			"error: interrupted (10)\n",
			"ok\n",
		},
		onSend: func(line string) {
			if line == "forever [ 1 ]" {
				tracker.request()
			}
		},
	}
	var out strings.Builder

	err := runSerialWithInterrupts(strings.NewReader("forever [ 1 ]\nblink:\n"), &out, dev, time.Second, tracker, false)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(dev.sent, "\n"), "forever [ 1 ]\nblink:"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	if got, want := out.String(), "frothy> error: interrupted (10)\nfrothy> ok\nfrothy> "; got != want {
		t.Fatalf("output %q, want %q", got, want)
	}
}

func TestSerialFileInterruptContinues(t *testing.T) {
	for _, test := range []struct {
		name       string
		hostSignal bool
		response   string
	}{
		{name: "host signal", hostSignal: true, response: "ok\n"},
		{name: "device button", response: "error: interrupted (10)\n"},
	} {
		t.Run(test.name, func(t *testing.T) {
			tracker := &interruptTracker{}
			dev := &fakeDevice{
				responses: []string{
					test.response,
					"ok\n",
				},
				onSend: func(line string) {
					if test.hostSignal && line == "forever [ 1 ]" {
						tracker.request()
					}
				},
			}
			var out strings.Builder

			err := runSerialWithInterrupts(strings.NewReader("forever [ 1 ]\nblink:\n"), &out, dev, time.Second, tracker, true)
			if err != nil {
				t.Fatal(err)
			}
			if got, want := strings.Join(dev.sent, "\n"), "forever [ 1 ]\nblink:"; got != want {
				t.Fatalf("sent %q, want %q", got, want)
			}
		})
	}
}

func TestPromptRecoveredAfterInterrupt(t *testing.T) {
	if !responseSettledAfterInterrupt("o" + "k\n") {
		t.Fatal("responseSettledAfterInterrupt did not combine partial ok")
	}
	if !responseSettledAfterInterrupt("noise\nerror: interrupted (" + "10)\n") {
		t.Fatal("responseSettledAfterInterrupt did not combine partial interrupt")
	}
	if !responseSettledAfterInterrupt("tick\ntick\nti" + "ck\nerror: interrupted (10)\n") {
		t.Fatal("responseSettledAfterInterrupt did not handle interrupted output before status")
	}
	if !promptRecoveredAfterInterrupt("tic", "error: interrupted (10)\n") {
		t.Fatal("promptRecoveredAfterInterrupt let partial output poison recovery")
	}
	if !promptRecoveredAfterInterrupt("error: interrupted (", "10)\n") {
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

func runRecordsTestSession(t *testing.T, input io.Reader, output io.Writer, dev sessionDevice, timeout time.Duration, tracker *interruptTracker) error {
	t.Helper()
	records := newRecordWriter(output, "s1")
	if err := records.sessionStart(); err != nil {
		t.Fatal(err)
	}
	status, err := readDeviceStatus(dev, timeout)
	if err != nil {
		_ = records.sessionError(recordStateError, recordErrorStatusFailed, err.Error())
		return err
	}
	if err := records.status(status); err != nil {
		t.Fatal(err)
	}
	return runSerialRecords(input, records, dev, timeout, tracker, false)
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
	if err := records.sessionEnd(); err != nil {
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
		`{"v":1,"session":"s1","seq":5,"kind":"compile_error","state":"idle","mirror":"none","source":"bad is fn [ pin: ]","reason":"source","status":"error: invalid (8)","text":"error: invalid (8)\n"}`,
		`{"v":1,"session":"s1","seq":6,"kind":"send","state":"waiting","mirror":"none","source":"bad:","line":"bad:","action":"direct"}`,
		`{"v":1,"session":"s1","seq":7,"kind":"response","state":"idle","mirror":"none","status":"error: unsupported (9)","ok":false,"text":"error: unsupported (9)\n"}`,
		`{"v":1,"session":"s1","seq":8,"kind":"send","state":"waiting","mirror":"none","source":"forever [ 1 ]","line":"forever [ 1 ]","action":"direct"}`,
		`{"v":1,"session":"s1","seq":9,"kind":"interrupt","state":"idle","mirror":"none","settled":true,"status":"error: interrupted (10)","text":"error: interrupted (10)\n"}`,
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
				`{"v":1,"session":"s1","seq":2,"kind":"interrupt","state":"stale","mirror":"stale","settled":true,"status":"error: interrupted (10)","text":"error: interrupted (10)\n"}`,
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

func TestReplayLinesFromNoticeResponseKeepsAcceptedSource(t *testing.T) {
	var out strings.Builder
	records := newRecordWriter(&out, "s1")
	if err := records.sessionStart(); err != nil {
		t.Fatal(err)
	}
	status, err := parseDeviceStatus(statusResponse("device"))
	if err != nil {
		t.Fatal(err)
	}
	if err := records.status(status); err != nil {
		t.Fatal(err)
	}
	if err := records.send("words"); err != nil {
		t.Fatal(err)
	}
	noticeResponse := "notice: not saved (13)\n" +
		"detail: cannot save a live handle\n" +
		"ok\n"
	if err := records.response(noticeResponse); err != nil {
		t.Fatal(err)
	}
	if err := records.sessionEnd(); err != nil {
		t.Fatal(err)
	}

	lines, err := replayLinesFromTranscript(strings.NewReader(out.String()))
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(lines, "\n"), "words"; got != want {
		t.Fatalf("replay lines %q, want %q", got, want)
	}
	response := recordWithKind(decodeRecords(t, out.String()), "response")
	if response["ok"] != true || response["notice"] != "notice: not saved (13)" ||
		response["text"] != noticeResponse {
		t.Fatalf("notice response record = %#v", response)
	}
}

func TestReplayLinesUseNormalSerialSessionPath(t *testing.T) {
	transcript := strings.Join([]string{
		`{"v":1,"session":"s1","seq":1,"kind":"session_start","state":"syncing","mirror":"none"}`,
		`{"v":1,"session":"s1","seq":2,"kind":"status","state":"idle","mirror":"none"}`,
		`{"v":1,"session":"s1","seq":3,"kind":"send","state":"waiting","mirror":"none","source":"time is 200","line":"time is 200","action":"direct"}`,
		`{"v":1,"session":"s1","seq":4,"kind":"response","state":"idle","mirror":"none","status":"ok","ok":true,"text":"ok\n"}`,
		`{"v":1,"session":"s1","seq":5,"kind":"send","state":"waiting","mirror":"none","source":"blink:","line":"blink:","action":"direct"}`,
		`{"v":1,"session":"s1","seq":6,"kind":"response","state":"idle","mirror":"none","status":"ok","ok":true,"text":"ok\n"}`,
		`{"v":1,"session":"s1","seq":7,"kind":"session_end","state":"closed","mirror":"none"}`,
	}, "\n") + "\n"
	lines, err := replayLinesFromTranscript(strings.NewReader(transcript))
	if err != nil {
		t.Fatal(err)
	}

	dev := &fakeDevice{
		responses: []string{
			statusResponse("device"),
			"ok\n",
			"ok\n",
		},
	}
	var out strings.Builder

	err = runSerial(readerFromLines(lines), &out, dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\ntime is 200\nblink:"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
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
		records    bool
		transcript string
		replay     string
		wantCode   int
		wantError  string
	}{
		{
			name:       "transcript without records",
			transcript: other,
			wantCode:   2,
			wantError:  "--transcript requires --records",
		},
		{
			name:      "replay file",
			filePath:  "main.fr",
			replay:    replay,
			wantCode:  2,
			wantError: "--replay cannot be combined with --file",
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
			code, err := validateSessionOptions(test.filePath, test.records, test.transcript, test.replay)
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

func TestRecordsGroupMultilineTopForm(t *testing.T) {
	dev := &fakeDevice{responses: []string{statusResponse("device"), "ok\n"}}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("boot is fn [\n  one\n]\n"), &out, dev, time.Second, &interruptTracker{})
	if err != nil {
		t.Fatal(err)
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,send,response,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	send := recordWithKind(records, "send")
	if send["source"] != "boot is fn [\n  one\n]" || send["line"] != "boot is fn [\n  one\n]" {
		t.Fatalf("send record = %#v", send)
	}
}

func TestRecordsSourceBlockEndsAfterAllExpandedForms(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "helper.fr"), []byte("helper is fn [ 1 ]\n"), 0644); err != nil {
		t.Fatal(err)
	}
	rootPath := filepath.Join(dir, "unsaved-main.fr")
	input := strings.NewReader(strings.Join([]string{
		".source " + rootPath,
		"include \"helper.fr\"",
		"main is fn [ helper: ]",
		".end-source",
		"",
	}, "\n"))
	dev := &fakeDevice{responses: []string{
		statusResponse("device"),
		"notice: not saved (13)\ndetail: still live\nok\n",
		"ok\n",
	}}
	var out strings.Builder

	if err := runRecordsTestSession(t, input, &out, dev, time.Second, &interruptTracker{}); err != nil {
		t.Fatal(err)
	}
	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,send,response,send,response,source_end,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	end := recordWithKind(records, "source_end")
	if end["state"] != "idle" || end["mirror"] != "none" {
		t.Fatalf("source_end record = %#v", end)
	}
	response := recordWithKind(records, "response")
	if response["ok"] != true || response["notice"] != "notice: not saved (13)" {
		t.Fatalf("notice stopped or was lost from source block: %#v", response)
	}
}

func TestRecordsEmptySourceBlockStillEnds(t *testing.T) {
	dev := &fakeDevice{responses: []string{statusResponse("device")}}
	var out strings.Builder
	input := strings.NewReader(".source\n-- no forms\n.end-source\n")

	if err := runRecordsTestSession(t, input, &out, dev, time.Second, &interruptTracker{}); err != nil {
		t.Fatal(err)
	}
	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,source_end,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
}

func TestRecordsSourceBlockInputErrorIsNotDeviceLost(t *testing.T) {
	dev := &fakeDevice{responses: []string{statusResponse("device")}}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader(".source main.fr\nmissing end\n"), &out, dev, time.Second, &interruptTracker{})
	if err == nil {
		t.Fatal("runRecordsTestSession accepted unterminated source block")
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,session_error"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	sessionError := recordWithKind(records, "session_error")
	if sessionError["code"] != "source_failed" || sessionError["message"] != ".source block missing .end-source" {
		t.Fatalf("session_error record = %#v", sessionError)
	}
}

func TestRecordsDeviceCompilerDirectSend(t *testing.T) {
	dev := &fakeDevice{responses: []string{statusResponse("device"), "ok\n"}}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("words\n"), &out, dev, time.Second, &interruptTracker{})
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

func TestRecordsFileStopsOnDeviceError(t *testing.T) {
	dev := &fakeDevice{responses: []string{
		statusResponse("device"),
		"error: not found (7)\nname: ok\nok\n^^\n",
		"ok\n",
	}}
	var out strings.Builder
	records := newRecordWriter(&out, "s1")
	if err := records.sessionStart(); err != nil {
		t.Fatal(err)
	}
	status, err := readDeviceStatus(dev, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if err := records.status(status); err != nil {
		t.Fatal(err)
	}

	err = runSerialRecords(strings.NewReader("ok\n2 + 2\n"), records, dev, time.Second, &interruptTracker{}, true)
	if err == nil || err.Error() != "device returned error: not found (7)" {
		t.Fatalf("error = %v, want device response error", err)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nok"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
	decoded := decodeRecords(t, out.String())
	if got, want := recordKinds(decoded), "session_start,status,send,response,session_error"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	sessionError := recordWithKind(decoded, "session_error")
	if sessionError["code"] != "source_failed" || sessionError["message"] != "device returned error: not found (7)" {
		t.Fatalf("session_error record = %#v", sessionError)
	}
}

func TestRecordsForegroundTimeoutInterruptsAndContinues(t *testing.T) {
	dev := &fakeDevice{
		responses: []string{
			statusResponse("device"),
			"error: interrupted (",
			"ok\n",
		},
		responseErrs: []error{nil, errPromptTimeout, nil},
		interruptResponses: []string{
			"10)\n",
		},
	}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("forever [ 1 ]\nblink:\n"), &out, dev, time.Second, &interruptTracker{})
	if err != nil {
		t.Fatal(err)
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,send,interrupt,send,response,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	interrupt := recordWithKind(records, "interrupt")
	if interrupt["state"] != "idle" || interrupt["mirror"] != "none" ||
		interrupt["settled"] != true || interrupt["status"] != "error: interrupted (10)" {
		t.Fatalf("interrupt record = %#v", interrupt)
	}
	if got, want := strings.Join(dev.sent, "\n"), "status\nforever [ 1 ]\nblink:"; got != want {
		t.Fatalf("sent %q, want %q", got, want)
	}
}

func TestRecordsFileInterruptContinues(t *testing.T) {
	for _, test := range []struct {
		name       string
		hostSignal bool
		response   string
		wantStatus string
	}{
		{name: "host signal", hostSignal: true, response: "ok\n", wantStatus: "ok"},
		{name: "device button", response: "error: interrupted (10)\n", wantStatus: "error: interrupted (10)"},
	} {
		t.Run(test.name, func(t *testing.T) {
			tracker := &interruptTracker{}
			dev := &fakeDevice{
				responses: []string{
					statusResponse("device"),
					test.response,
					"ok\n",
				},
				onSend: func(line string) {
					if test.hostSignal && line == "forever [ 1 ]" {
						tracker.request()
					}
				},
			}
			var out strings.Builder
			writer := newRecordWriter(&out, "s1")
			if err := writer.sessionStart(); err != nil {
				t.Fatal(err)
			}
			status, err := readDeviceStatus(dev, time.Second)
			if err != nil {
				t.Fatal(err)
			}
			if err := writer.status(status); err != nil {
				t.Fatal(err)
			}

			err = runSerialRecords(strings.NewReader("forever [ 1 ]\nblink:\n"), writer, dev, time.Second, tracker, true)
			if err != nil {
				t.Fatal(err)
			}

			records := decodeRecords(t, out.String())
			if got, want := recordKinds(records), "session_start,status,send,interrupt,send,response,session_end"; got != want {
				t.Fatalf("record kinds %q, want %q", got, want)
			}
			interrupt := recordWithKind(records, "interrupt")
			if interrupt["state"] != "idle" || interrupt["mirror"] != "none" ||
				interrupt["settled"] != true || interrupt["status"] != test.wantStatus {
				t.Fatalf("interrupt record = %#v", interrupt)
			}
			if got, want := strings.Join(dev.sent, "\n"), "status\nforever [ 1 ]\nblink:"; got != want {
				t.Fatalf("sent %q, want %q", got, want)
			}
		})
	}
}

func TestRecordsUnsettledSignalInterruptFails(t *testing.T) {
	tracker := &interruptTracker{}
	dev := &fakeDevice{
		responses: []string{
			statusResponse("device"),
			"boot banner\n",
		},
		onSend: func(line string) {
			if line == "forever [ 1 ]" {
				tracker.request()
			}
		},
	}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("forever [ 1 ]\n"), &out, dev, time.Second, tracker)
	if err == nil {
		t.Fatal("runRecordsTestSession accepted unsettled interrupt")
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,send,interrupt,session_error"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}
	interrupt := recordWithKind(records, "interrupt")
	if interrupt["state"] != "error" || interrupt["mirror"] != "none" ||
		interrupt["settled"] != false || interrupt["code"] != "interrupt_failed" {
		t.Fatalf("interrupt record = %#v", interrupt)
	}
	sessionError := recordWithKind(records, "session_error")
	if sessionError["state"] != "error" || sessionError["mirror"] != "none" ||
		sessionError["code"] != "interrupt_failed" {
		t.Fatalf("session_error record = %#v", sessionError)
	}
}
