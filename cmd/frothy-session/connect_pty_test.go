//go:build darwin || linux

package main

import (
	"bytes"
	"io"
	"strings"
	"sync"
	"testing"
	"time"

	"frothyrewrite/internal/testpty"
)

// User types "foo" without Enter; device emits a notice line; after the
// idle window the input line is erased, the notice prints, and the
// prompt + buffered "foo" are redrawn at the right cursor column.
func TestConnectRedrawAfterDeviceOutputMidTyping(t *testing.T) {
	master, slave, _, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	if err := configureSerial(slave, 115200); err != nil {
		t.Fatal(err)
	}
	defer master.Close()
	defer slave.Close()

	dev := &serialDevice{
		file:   slave,
		readCh: make(chan byte, 256),
		errCh:  make(chan error, 1),
	}
	go dev.readLoop()

	stdinR, stdinW := io.Pipe()
	defer stdinR.Close()

	var stdoutMu sync.Mutex
	var stdout bytes.Buffer
	syncedStdout := &lockedWriter{w: &stdout, mu: &stdoutMu}

	done := make(chan int, 1)
	go func() {
		done <- runConnectInteractive(dev, stdinR, syncedStdout, connectHistory{})
	}()

	if _, err := stdinW.Write([]byte("foo")); err != nil {
		t.Fatal(err)
	}
	waitForStdoutContains(t, &stdoutMu, &stdout, promptPrimary+"foo", time.Second)

	if _, err := master.Write([]byte("notice\r\n")); err != nil {
		t.Fatal(err)
	}
	waitForStdoutContains(t, &stdoutMu, &stdout, "notice", time.Second)

	// Wait until the redrawn prompt+buf appears after the notice.
	waitForStdoutMatches(t, &stdoutMu, &stdout, func(s string) bool {
		i := strings.Index(s, "notice")
		if i < 0 {
			return false
		}
		return strings.Contains(s[i:], promptPrimary+"foo")
	}, time.Second)

	if _, err := stdinW.Write([]byte{0x04}); err != nil {
		t.Fatal(err)
	}
	_ = stdinW.Close()

	select {
	case code := <-done:
		if code != 0 {
			t.Fatalf("exit code = %d, want 0", code)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("runConnectInteractive did not return")
	}

	stdoutMu.Lock()
	out := stdout.String()
	stdoutMu.Unlock()

	// The pending input line must be erased before "notice" prints.
	noticeIdx := strings.Index(out, "notice")
	if noticeIdx < 0 {
		t.Fatalf("notice missing: %q", out)
	}
	if !strings.Contains(out[:noticeIdx], "\r\x1b[K") {
		t.Fatalf("input line not erased before notice: %q", out)
	}
	// "foo" must appear twice: once when typed locally, once when redrawn.
	if strings.Count(out, "foo") < 2 {
		t.Fatalf("expected 'foo' twice (local type + redraw), got: %q", out)
	}
}

func TestConnectSubmitSwallowsDeviceEchoAndGatesPrompt(t *testing.T) {
	master, slave, _, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	if err := configureSerial(slave, 115200); err != nil {
		t.Fatal(err)
	}
	defer master.Close()
	defer slave.Close()

	dev := &serialDevice{
		file:   slave,
		readCh: make(chan byte, 256),
		errCh:  make(chan error, 1),
	}
	go dev.readLoop()

	stdinR, stdinW := io.Pipe()
	defer stdinR.Close()

	var stdoutMu sync.Mutex
	var stdout bytes.Buffer
	syncedStdout := &lockedWriter{w: &stdout, mu: &stdoutMu}

	done := make(chan int, 1)
	go func() {
		done <- runConnectInteractive(dev, stdinR, syncedStdout, connectHistory{})
	}()

	waitForStdoutContains(t, &stdoutMu, &stdout, promptPrimary, time.Second)

	deviceRx := readChan(master)
	if _, err := stdinW.Write([]byte("1 + 2\r")); err != nil {
		t.Fatal(err)
	}

	wantSent := []byte("1 + 2\n")
	gotSent := make([]byte, 0, len(wantSent))
	deadline := time.After(2 * time.Second)
	for len(gotSent) < len(wantSent) {
		select {
		case b := <-deviceRx:
			gotSent = append(gotSent, b)
		case <-deadline:
			t.Fatalf("device received %q, want %q", gotSent, wantSent)
		}
	}
	if !bytes.Equal(gotSent, wantSent) {
		t.Fatalf("device received %q, want %q", gotSent, wantSent)
	}

	time.Sleep(50 * time.Millisecond)
	stdoutMu.Lock()
	beforeDevice := stdout.String()
	stdoutMu.Unlock()
	if strings.Count(beforeDevice, promptPrimary) != 1 {
		t.Fatalf("host drew prompt before device was ready: %q", beforeDevice)
	}
	if !strings.Contains(beforeDevice, promptPrimary+"1 + 2"+terminalLineBreak) {
		t.Fatalf("submit did not emit host CRLF before device response: %q", beforeDevice)
	}

	if _, err := master.Write([]byte("1 + 2\r\n3\r\nok\r\n> ")); err != nil {
		t.Fatal(err)
	}
	waitForStdoutMatches(t, &stdoutMu, &stdout, func(s string) bool {
		return strings.HasSuffix(s, promptPrimary)
	}, time.Second)

	stdoutMu.Lock()
	out := stdout.String()
	stdoutMu.Unlock()
	if strings.Count(out, "1 + 2") != 1 {
		t.Fatalf("submitted source rendered more than once: %q", out)
	}
	wantRendered := promptPrimary + "1 + 2" + terminalLineBreak + "3\r\nok\r\n" + promptPrimary
	if !strings.Contains(out, wantRendered) {
		t.Fatalf("response did not start at column 0 after host CRLF: %q", out)
	}
	sourceIdx := strings.Index(out, "1 + 2")
	resultIdx := strings.Index(out, "3\r\nok\r\n")
	if sourceIdx < 0 || resultIdx < 0 || sourceIdx >= resultIdx {
		t.Fatalf("source/result order wrong: %q", out)
	}
	between := out[sourceIdx+len("1 + 2") : resultIdx]
	if strings.Contains(between, promptPrimary) {
		t.Fatalf("host prompt appeared before device result: %q", out)
	}
	if strings.Contains(out, "\r\n> ") {
		t.Fatalf("raw device prompt rendered: %q", out)
	}

	if _, err := stdinW.Write([]byte{0x04}); err != nil {
		t.Fatal(err)
	}
	_ = stdinW.Close()

	select {
	case code := <-done:
		if code != 0 {
			t.Fatalf("exit code = %d, want 0", code)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("runConnectInteractive did not return")
	}
}

func TestConnectPipedOutputUsesPlainText(t *testing.T) {
	master, slave, _, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	if err := configureSerial(slave, 115200); err != nil {
		t.Fatal(err)
	}
	defer master.Close()
	defer slave.Close()

	dev := &serialDevice{
		file:   slave,
		readCh: make(chan byte, 256),
		errCh:  make(chan error, 1),
	}
	go dev.readLoop()

	stdinR, stdinW := io.Pipe()
	defer stdinR.Close()

	var stdoutMu sync.Mutex
	var stdout bytes.Buffer
	syncedStdout := &lockedWriter{w: &stdout, mu: &stdoutMu}

	done := make(chan int, 1)
	go func() {
		done <- runConnectInteractiveTermios(dev, stdinR, syncedStdout, connectHistory{}, nil)
	}()

	waitForStdoutContains(t, &stdoutMu, &stdout, promptPrimary, time.Second)

	deviceRx := readChan(master)
	if _, err := stdinW.Write([]byte("1 + 2\n")); err != nil {
		t.Fatal(err)
	}

	wantSent := []byte("1 + 2\n")
	gotSent := make([]byte, 0, len(wantSent))
	deadline := time.After(2 * time.Second)
	for len(gotSent) < len(wantSent) {
		select {
		case b := <-deviceRx:
			gotSent = append(gotSent, b)
		case <-deadline:
			t.Fatalf("device received %q, want %q", gotSent, wantSent)
		}
	}
	if !bytes.Equal(gotSent, wantSent) {
		t.Fatalf("device received %q, want %q", gotSent, wantSent)
	}

	if _, err := master.Write([]byte("1 + 2\n3\nok\n> ")); err != nil {
		t.Fatal(err)
	}
	waitForStdoutMatches(t, &stdoutMu, &stdout, func(s string) bool {
		return strings.HasSuffix(s, promptPrimary)
	}, time.Second)

	stdoutMu.Lock()
	out := stdout.String()
	stdoutMu.Unlock()
	if strings.Contains(out, "\x1b[") {
		t.Fatalf("piped output contains terminal escape sequence: %q", out)
	}
	if strings.Contains(out, "\r\n") {
		t.Fatalf("piped output contains terminal CRLF: %q", out)
	}
	want := promptPrimary + "1 + 2\n3\nok\n" + promptPrimary
	if !strings.Contains(out, want) {
		t.Fatalf("piped output = %q, want substring %q", out, want)
	}

	if _, err := stdinW.Write([]byte{0x04}); err != nil {
		t.Fatal(err)
	}
	_ = stdinW.Close()

	select {
	case code := <-done:
		if code != 0 {
			t.Fatalf("exit code = %d, want 0", code)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("runConnectInteractiveTermios did not return")
	}
}

// User types Ctrl-C with no buffered input; the device sees byte 0x03 on
// the serial side and a second Ctrl-C inside the window exits the
// interactive loop with code 0.
func TestConnectInterruptForwardsAndDoubleExits(t *testing.T) {
	master, slave, _, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	if err := configureSerial(slave, 115200); err != nil {
		t.Fatal(err)
	}
	defer master.Close()
	defer slave.Close()

	dev := &serialDevice{
		file:   slave,
		readCh: make(chan byte, 256),
		errCh:  make(chan error, 1),
	}
	go dev.readLoop()

	stdinR, stdinW := io.Pipe()
	defer stdinR.Close()

	var stdoutMu sync.Mutex
	var stdout bytes.Buffer
	syncedStdout := &lockedWriter{w: &stdout, mu: &stdoutMu}

	done := make(chan int, 1)
	go func() {
		done <- runConnectInteractive(dev, stdinR, syncedStdout, connectHistory{})
	}()

	waitForStdoutContains(t, &stdoutMu, &stdout, promptPrimary, time.Second)

	deviceRx := readChan(master)

	if _, err := stdinW.Write([]byte{0x03}); err != nil {
		t.Fatal(err)
	}
	select {
	case b := <-deviceRx:
		if b != 0x03 {
			t.Fatalf("device byte = %#x, want 0x03", b)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("device did not receive interrupt byte")
	}

	if _, err := stdinW.Write([]byte{0x03}); err != nil {
		t.Fatal(err)
	}
	_ = stdinW.Close()

	select {
	case code := <-done:
		if code != 0 {
			t.Fatalf("exit code = %d, want 0", code)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("double-Ctrl-C did not exit")
	}
}

// Empty-buffer Ctrl-C forwards 0x03 to the device and then waits for the
// device's "> " prompt (via the pump's devicePrompt event) before redrawing
// the client prompt. The proof is in the stdout order: no client redraw
// between the initial prompt and the device's recovery output.
func TestConnectInterruptEmptyWaitsForDevicePrompt(t *testing.T) {
	master, slave, _, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	if err := configureSerial(slave, 115200); err != nil {
		t.Fatal(err)
	}
	defer master.Close()
	defer slave.Close()

	dev := &serialDevice{
		file:   slave,
		readCh: make(chan byte, 256),
		errCh:  make(chan error, 1),
	}
	go dev.readLoop()

	stdinR, stdinW := io.Pipe()
	defer stdinR.Close()

	var stdoutMu sync.Mutex
	var stdout bytes.Buffer
	syncedStdout := &lockedWriter{w: &stdout, mu: &stdoutMu}

	done := make(chan int, 1)
	go func() {
		done <- runConnectInteractive(dev, stdinR, syncedStdout, connectHistory{})
	}()

	waitForStdoutContains(t, &stdoutMu, &stdout, promptPrimary, time.Second)

	deviceRx := readChan(master)

	if _, err := stdinW.Write([]byte{0x03}); err != nil {
		t.Fatal(err)
	}
	select {
	case b := <-deviceRx:
		if b != 0x03 {
			t.Fatalf("device byte = %#x, want 0x03", b)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("device did not receive interrupt byte")
	}

	if _, err := master.Write([]byte("ok\r\n> ")); err != nil {
		t.Fatal(err)
	}

	waitForStdoutMatches(t, &stdoutMu, &stdout, func(s string) bool {
		i := strings.Index(s, "ok\r\n")
		return i >= 0 && strings.Contains(s[i:], "\r\x1b[K"+promptPrimary)
	}, time.Second)

	stdoutMu.Lock()
	out := stdout.String()
	stdoutMu.Unlock()

	firstPromptEnd := strings.Index(out, promptPrimary) + len(promptPrimary)
	okIdx := strings.Index(out, "ok\r\n")
	if okIdx < 0 {
		t.Fatalf("device output missing from stdout: %q", out)
	}
	if strings.Contains(out[firstPromptEnd:okIdx], "\r\x1b[K") {
		t.Fatalf("client redrew before device's prompt arrived: %q", out)
	}

	if _, err := stdinW.Write([]byte{0x04}); err != nil {
		t.Fatal(err)
	}
	_ = stdinW.Close()

	select {
	case code := <-done:
		if code != 0 {
			t.Fatalf("exit code = %d, want 0", code)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Ctrl-D did not exit")
	}
}

// User types a line then Ctrl-C; the buffer clears (so the device never
// sees 0x03), and a follow-up Ctrl-D on the now-empty buffer exits.
func TestConnectInterruptWithBufferClearsThenEOFExits(t *testing.T) {
	master, slave, _, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	if err := configureSerial(slave, 115200); err != nil {
		t.Fatal(err)
	}
	defer master.Close()
	defer slave.Close()

	dev := &serialDevice{
		file:   slave,
		readCh: make(chan byte, 256),
		errCh:  make(chan error, 1),
	}
	go dev.readLoop()

	stdinR, stdinW := io.Pipe()
	defer stdinR.Close()

	var stdoutMu sync.Mutex
	var stdout bytes.Buffer
	syncedStdout := &lockedWriter{w: &stdout, mu: &stdoutMu}

	done := make(chan int, 1)
	go func() {
		done <- runConnectInteractive(dev, stdinR, syncedStdout, connectHistory{})
	}()

	if _, err := stdinW.Write([]byte("foo")); err != nil {
		t.Fatal(err)
	}
	waitForStdoutContains(t, &stdoutMu, &stdout, promptPrimary+"foo", time.Second)

	if _, err := stdinW.Write([]byte{0x03}); err != nil {
		t.Fatal(err)
	}
	waitForStdoutMatches(t, &stdoutMu, &stdout, func(s string) bool {
		i := strings.LastIndex(s, "\r\x1b[K")
		return i >= 0 && strings.HasSuffix(s[i:], "\r\x1b[K"+promptPrimary)
	}, time.Second)

	deviceRx := readChan(master)
	select {
	case b := <-deviceRx:
		t.Fatalf("device received byte after buf-clear Ctrl-C: %#x", b)
	case <-time.After(100 * time.Millisecond):
	}

	if _, err := stdinW.Write([]byte{0x04}); err != nil {
		t.Fatal(err)
	}
	_ = stdinW.Close()

	select {
	case code := <-done:
		if code != 0 {
			t.Fatalf("exit code = %d, want 0", code)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Ctrl-D after buf-clear did not exit")
	}
}

// The pump detects the ESP boot banner and emits start/end events; the
// client renders a reset notice when the first banner line completes and a
// ready notice after the next prompt. Non-banner output (an `ok` ack or a
// line that merely mentions `rst:0x` mid-text) must not trigger either.
func TestConnectResetDetectionRendersNotices(t *testing.T) {
	master, slave, _, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	if err := configureSerial(slave, 115200); err != nil {
		t.Fatal(err)
	}
	defer master.Close()
	defer slave.Close()

	dev := &serialDevice{
		file:   slave,
		readCh: make(chan byte, 256),
		errCh:  make(chan error, 1),
	}
	go dev.readLoop()

	stdinR, stdinW := io.Pipe()
	defer stdinR.Close()

	var stdoutMu sync.Mutex
	var stdout bytes.Buffer
	syncedStdout := &lockedWriter{w: &stdout, mu: &stdoutMu}

	done := make(chan int, 1)
	go func() {
		done <- runConnectInteractive(dev, stdinR, syncedStdout, connectHistory{})
	}()

	waitForStdoutContains(t, &stdoutMu, &stdout, promptPrimary, time.Second)

	negatives := "ok\r\nlook at rst:0x stuff\r\nvalue is 42\r\n> "
	if _, err := master.Write([]byte(negatives)); err != nil {
		t.Fatal(err)
	}
	waitForStdoutContains(t, &stdoutMu, &stdout, "value is 42\r\n"+promptPrimary, time.Second)

	stdoutMu.Lock()
	before := stdout.String()
	beforeLen := len(before)
	stdoutMu.Unlock()
	if strings.Contains(before, "-- device reset detected") || strings.Contains(before, "-- prompt restored --") {
		t.Fatalf("non-banner output triggered reset notices: %q", before)
	}

	banner := "ets Jul 29 2019 12:21:46\r\n" +
		"\r\n" +
		"rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)\r\n" +
		"configsip: 0, SPIWP:0xee\r\n" +
		"clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00\r\n" +
		"mode:DIO, clock div:2\r\n" +
		"load:0x3fff0030,len:1184\r\n" +
		"load:0x40078000,len:13076\r\n" +
		"load:0x40080400,len:3076\r\n" +
		"entry 0x400805f0\r\n" +
		"> "
	if _, err := master.Write([]byte(banner)); err != nil {
		t.Fatal(err)
	}

	waitForStdoutContains(t, &stdoutMu, &stdout, "-- device reset detected; waiting for prompt --", time.Second)
	waitForStdoutContains(t, &stdoutMu, &stdout, "-- prompt restored --", time.Second)

	stdoutMu.Lock()
	after := stdout.String()
	stdoutMu.Unlock()
	resetTail := after[beforeLen:]
	noticeIdx := strings.Index(resetTail, "-- device reset detected; waiting for prompt --")
	restoredIdx := strings.Index(resetTail, "-- prompt restored --")
	entryIdx := strings.Index(resetTail, "entry 0x400805f0")
	recoveryPromptIdx := strings.Index(resetTail, promptPrimary)
	if noticeIdx < 0 || restoredIdx < 0 || entryIdx < 0 || recoveryPromptIdx < 0 {
		t.Fatalf("reset segments missing: notice=%d restored=%d entry=%d recoveryPrompt=%d in %q",
			noticeIdx, restoredIdx, entryIdx, recoveryPromptIdx, resetTail)
	}
	if noticeIdx >= recoveryPromptIdx {
		t.Fatalf("reset notice did not precede recovery host prompt: notice=%d recoveryPrompt=%d in %q",
			noticeIdx, recoveryPromptIdx, resetTail)
	}
	if noticeIdx >= entryIdx {
		t.Fatalf("reset notice did not precede banner tail: notice=%d entry=%d in %q",
			noticeIdx, entryIdx, resetTail)
	}
	if restoredIdx <= entryIdx {
		t.Fatalf("restored notice did not follow banner tail: restored=%d entry=%d in %q",
			restoredIdx, entryIdx, resetTail)
	}

	if _, err := stdinW.Write([]byte("bar")); err != nil {
		t.Fatal(err)
	}
	waitForStdoutMatches(t, &stdoutMu, &stdout, func(s string) bool {
		i := strings.Index(s, "-- prompt restored --")
		return i >= 0 && strings.Contains(s[i:], "bar")
	}, time.Second)

	if _, err := stdinW.Write([]byte{0x03}); err != nil {
		t.Fatal(err)
	}
	if _, err := stdinW.Write([]byte{0x04}); err != nil {
		t.Fatal(err)
	}
	_ = stdinW.Close()

	select {
	case code := <-done:
		if code != 0 {
			t.Fatalf("exit code = %d, want 0", code)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("exit did not return")
	}
}

// A bracket-balanced form typed across three lines feeds through
// sourceFormState: the device receives one escaped source-form request, and the user
// sees a continuation prompt for each interior line.
func TestConnectMultiLineContinuationSubmitsOneForm(t *testing.T) {
	master, slave, _, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	if err := configureSerial(slave, 115200); err != nil {
		t.Fatal(err)
	}
	defer master.Close()
	defer slave.Close()

	dev := &serialDevice{
		file:   slave,
		readCh: make(chan byte, 256),
		errCh:  make(chan error, 1),
	}
	go dev.readLoop()

	stdinR, stdinW := io.Pipe()
	defer stdinR.Close()

	var stdoutMu sync.Mutex
	var stdout bytes.Buffer
	syncedStdout := &lockedWriter{w: &stdout, mu: &stdoutMu}

	done := make(chan int, 1)
	go func() {
		done <- runConnectInteractive(dev, stdinR, syncedStdout, connectHistory{})
	}()

	waitForStdoutContains(t, &stdoutMu, &stdout, promptPrimary, time.Second)

	deviceRx := readChan(master)

	if _, err := stdinW.Write([]byte("to foo with x [\r")); err != nil {
		t.Fatal(err)
	}
	waitForStdoutContains(t, &stdoutMu, &stdout, promptContinuation, time.Second)

	if _, err := stdinW.Write([]byte("gpio.high: x\r")); err != nil {
		t.Fatal(err)
	}
	waitForStdoutMatches(t, &stdoutMu, &stdout, func(s string) bool {
		return strings.Count(s, promptContinuation) >= 2
	}, time.Second)

	if _, err := stdinW.Write([]byte("]\r")); err != nil {
		t.Fatal(err)
	}

	want := []byte("source-form to foo with x [\\ngpio.high: x\\n]\n")
	got := make([]byte, 0, len(want))
	deadline := time.After(2 * time.Second)
	for len(got) < len(want) {
		select {
		case b := <-deviceRx:
			got = append(got, b)
		case <-deadline:
			t.Fatalf("device received %q, want %q", got, want)
		}
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("device received %q, want %q", got, want)
	}

	deviceEcho := strings.TrimSuffix(string(want), "\n") + "\r\nok\r\n> "
	if _, err := master.Write([]byte(deviceEcho)); err != nil {
		t.Fatal(err)
	}

	waitForStdoutMatches(t, &stdoutMu, &stdout, func(s string) bool {
		return strings.HasSuffix(s, promptPrimary)
	}, time.Second)

	if _, err := stdinW.Write([]byte{0x04}); err != nil {
		t.Fatal(err)
	}
	_ = stdinW.Close()

	select {
	case code := <-done:
		if code != 0 {
			t.Fatalf("exit code = %d, want 0", code)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("runConnectInteractive did not return")
	}

	select {
	case b := <-deviceRx:
		t.Fatalf("device received unexpected trailing byte %#x", b)
	case <-time.After(50 * time.Millisecond):
	}
}

// readChan reads bytes from r forever and forwards them one by one. The
// goroutine exits when r returns an error (typically when the test
// closes the PTY).
func readChan(r io.Reader) <-chan byte {
	ch := make(chan byte, 64)
	go func() {
		buf := make([]byte, 64)
		for {
			n, err := r.Read(buf)
			for i := 0; i < n; i++ {
				ch <- buf[i]
			}
			if err != nil {
				return
			}
		}
	}()
	return ch
}

type lockedWriter struct {
	w  io.Writer
	mu *sync.Mutex
}

func (l *lockedWriter) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.w.Write(p)
}

func waitForStdoutContains(t *testing.T, mu *sync.Mutex, buf *bytes.Buffer, want string, limit time.Duration) {
	t.Helper()
	waitForStdoutMatches(t, mu, buf, func(s string) bool { return strings.Contains(s, want) }, limit)
}

func waitForStdoutMatches(t *testing.T, mu *sync.Mutex, buf *bytes.Buffer, ok func(string) bool, limit time.Duration) {
	t.Helper()
	deadline := time.Now().Add(limit)
	for time.Now().Before(deadline) {
		mu.Lock()
		s := buf.String()
		mu.Unlock()
		if ok(s) {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	mu.Lock()
	s := buf.String()
	mu.Unlock()
	t.Fatalf("timed out waiting for stdout match; got: %q", s)
}
