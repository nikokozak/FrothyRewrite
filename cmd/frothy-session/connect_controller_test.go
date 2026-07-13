//go:build darwin || linux

package main

import (
	"bytes"
	"io"
	"reflect"
	"strings"
	"testing"
	"time"
)

func TestConnectControllerSubmitFlushesPendingAndWaitsForDevicePrompt(t *testing.T) {
	var out bytes.Buffer
	var sent [][]byte
	c := newConnectController(&out, func(b []byte) error {
		sent = append(sent, append([]byte(nil), b...))
		return nil
	})

	c.writePrompt()
	if exit, _ := c.onInput(inputPrintable{Bytes: []byte("foo")}); exit {
		t.Fatal("early exit on printable")
	}
	if exit, _ := c.onDevice(deviceBytes{Bytes: []byte("notice\r\n")}); exit {
		t.Fatal("early exit on device bytes")
	}
	if !c.idleArmed {
		t.Fatal("idle should be armed while pending bytes wait behind the input line")
	}
	if exit, _ := c.onInput(inputSubmit{}); exit {
		t.Fatal("early exit on submit")
	}

	want := promptPrimary + "foo" + terminalLineBreak + "notice\r\n"
	if got := out.String(); got != want {
		t.Fatalf("stdout = %q, want %q", got, want)
	}
	if len(sent) != 1 || string(sent[0]) != "foo\n" {
		t.Fatalf("sent = %q, want [\"foo\\n\"]", sent)
	}
	if !c.awaitingPrompt {
		t.Fatal("submit should wait for the device prompt before drawing the next host prompt")
	}
	if c.idleArmed {
		t.Fatal("idle should be cleared after submit consumed pending")
	}

	if exit, _ := c.onDevice(deviceBytes{Bytes: []byte("foo\r\n3\r\nok\r\n")}); exit {
		t.Fatal("early exit on device bytes")
	}
	if exit, _ := c.onDevice(devicePrompt{}); exit {
		t.Fatal("early exit on device prompt")
	}
	want = promptPrimary + "foo" + terminalLineBreak + "notice\r\n3\r\nok\r\n" + promptPrimary
	if got := out.String(); got != want {
		t.Fatalf("stdout after device response = %q, want %q", got, want)
	}
}

func TestConnectControllerDeviceBytesWithEmptyBuf(t *testing.T) {
	var out bytes.Buffer
	c := newConnectController(&out, func([]byte) error { return nil })
	c.writePrompt()

	if exit, _ := c.onDevice(deviceBytes{Bytes: []byte("hello\r\n")}); exit {
		t.Fatal("early exit on device bytes")
	}
	if c.idleArmed {
		t.Fatal("idle should stay disarmed when buffer is empty")
	}
	if got := out.String(); got != promptPrimary+"hello\r\n" {
		t.Fatalf("stdout = %q", got)
	}
}

func TestConnectControllerPlainModeUsesLFForHostNotices(t *testing.T) {
	var out bytes.Buffer
	c := newConnectController(&out, func([]byte) error { return nil })
	c.terminal = false
	c.writePrompt()

	if exit, _ := c.onDevice(deviceResetStart{}); exit {
		t.Fatal("early exit on reset start")
	}
	if exit, _ := c.onDevice(deviceResetEnd{}); exit {
		t.Fatal("early exit on reset end")
	}

	got := out.String()
	want := promptPrimary +
		"-- device reset detected; waiting for prompt --\n" +
		"-- prompt restored --\n"
	if got != want {
		t.Fatalf("stdout = %q, want %q", got, want)
	}
	if strings.Contains(got, "\r\n") || strings.Contains(got, "\x1b[") {
		t.Fatalf("plain-mode host notice contains terminal output: %q", got)
	}
}

func TestConnectControllerSwallowsExpectedEchoAcrossChunks(t *testing.T) {
	var out bytes.Buffer
	c := newConnectController(&out, func([]byte) error { return nil })
	c.writePrompt()
	c.onInput(inputPrintable{Bytes: []byte("1 + 2")})
	c.onInput(inputSubmit{})

	c.onDevice(deviceBytes{Bytes: []byte("1 +")})
	c.onDevice(deviceBytes{Bytes: []byte(" 2\r")})
	c.onDevice(deviceBytes{Bytes: []byte("\n3\r\nok\r\n")})
	c.onDevice(devicePrompt{})

	want := promptPrimary + "1 + 2" + terminalLineBreak + "3\r\nok\r\n" + promptPrimary
	if got := out.String(); got != want {
		t.Fatalf("stdout = %q, want %q", got, want)
	}
	if len(c.expectedEchoes) != 0 || c.resolvedExpectedEchoes != 0 {
		t.Fatalf("echo queue not drained: expected=%d resolved=%d", len(c.expectedEchoes), c.resolvedExpectedEchoes)
	}
}

func TestConnectControllerEchoMismatchStopsSwallowing(t *testing.T) {
	var out bytes.Buffer
	c := newConnectController(&out, func([]byte) error { return nil })
	c.writePrompt()
	c.onInput(inputPrintable{Bytes: []byte("foo")})
	c.onInput(inputSubmit{})

	c.onDevice(deviceBytes{Bytes: []byte("food\r\nok\r\n")})
	c.onDevice(devicePrompt{})

	want := promptPrimary + "foo" + terminalLineBreak + "d\r\nok\r\n" + promptPrimary
	if got := out.String(); got != want {
		t.Fatalf("stdout = %q, want %q", got, want)
	}
}

func TestConnectControllerSwallowsMultipleExpectedEchoes(t *testing.T) {
	var out bytes.Buffer
	var sent [][]byte
	c := newConnectController(&out, func(b []byte) error {
		sent = append(sent, append([]byte(nil), b...))
		return nil
	})
	c.writePrompt()
	c.onInput(inputPrintable{Bytes: []byte("foo")})
	c.onInput(inputSubmit{})
	c.onInput(inputPrintable{Bytes: []byte("bar")})
	c.onInput(inputSubmit{})

	c.onDevice(deviceBytes{Bytes: []byte("foo\r\nbar\r\nok\r\n")})

	if len(sent) != 2 || string(sent[0]) != "foo\n" || string(sent[1]) != "bar\n" {
		t.Fatalf("sent = %q, want foo and bar", sent)
	}
	want := promptPrimary + "foo" + terminalLineBreak + "bar" + terminalLineBreak + "ok\r\n"
	if got := out.String(); got != want {
		t.Fatalf("stdout = %q, want %q", got, want)
	}
	if len(c.expectedEchoes) != 0 {
		t.Fatalf("expected echo queue length = %d, want 0", len(c.expectedEchoes))
	}
}

func TestConnectControllerSubmitRecordsHistory(t *testing.T) {
	c := newConnectController(io.Discard, func([]byte) error { return nil })
	c.historyOn = true
	c.writePrompt()

	for _, seq := range []string{"foo", "foo", "", "bar"} {
		if seq != "" {
			c.onInput(inputPrintable{Bytes: []byte(seq)})
		}
		c.onInput(inputSubmit{})
	}

	want := []string{"foo", "bar"}
	if !reflect.DeepEqual(c.history, want) {
		t.Fatalf("history = %q, want %q", c.history, want)
	}
}

func TestConnectControllerSendsMultilineFormWithoutFlatteningHistory(t *testing.T) {
	var sent [][]byte
	c := newConnectController(io.Discard, func(line []byte) error {
		sent = append(sent, append([]byte(nil), line...))
		return nil
	})
	c.historyOn = true
	c.writePrompt()

	for _, line := range []string{"to foo [", "  one", "]"} {
		c.onInput(inputPrintable{Bytes: []byte(line)})
		c.onInput(inputSubmit{})
	}

	if len(sent) != 1 || string(sent[0]) != "source-form to foo [\\n  one\\n]\n" {
		t.Fatalf("sent = %q, want one encoded source-form", sent)
	}
	if len(c.history) != 0 {
		t.Fatalf("multiline form entered one-line history: %q", c.history)
	}
}

func TestConnectControllerInsertMidLine(t *testing.T) {
	var out bytes.Buffer
	c := newConnectController(&out, func([]byte) error { return nil })
	c.writePrompt()
	c.onInput(inputPrintable{Bytes: []byte("foo")})
	c.onInput(inputCursorMove{Dir: cursorLeft})
	c.onInput(inputPrintable{Bytes: []byte("X")})

	if string(c.buf) != "foXo" || c.cursor != 3 {
		t.Fatalf("buf=%q cursor=%d, want foXo / 3", c.buf, c.cursor)
	}
	want := promptPrimary + "foo" +
		"\r\x1b[K" + promptPrimary + "foo" + "\x1b[1D" +
		"\r\x1b[K" + promptPrimary + "foXo" + "\x1b[1D"
	if got := out.String(); got != want {
		t.Fatalf("stdout = %q, want %q", got, want)
	}
}

func TestConnectControllerCursorHomeEnd(t *testing.T) {
	c := newConnectController(io.Discard, func([]byte) error { return nil })
	c.writePrompt()
	c.onInput(inputPrintable{Bytes: []byte("hello")})
	c.onInput(inputCursorMove{Dir: cursorHome})
	if c.cursor != 0 {
		t.Fatalf("after Home cursor=%d, want 0", c.cursor)
	}
	c.onInput(inputCursorMove{Dir: cursorEnd})
	if c.cursor != 5 {
		t.Fatalf("after End cursor=%d, want 5", c.cursor)
	}
	c.onInput(inputCursorMove{Dir: cursorRight})
	if c.cursor != 5 {
		t.Fatalf("Right past end advanced: cursor=%d", c.cursor)
	}
	c.onInput(inputCursorMove{Dir: cursorHome})
	c.onInput(inputCursorMove{Dir: cursorLeft})
	if c.cursor != 0 {
		t.Fatalf("Left past start advanced: cursor=%d", c.cursor)
	}
}

func TestConnectControllerErase(t *testing.T) {
	cases := []struct {
		name       string
		seed       string
		seedCursor int
		kind       eraseKind
		wantBuf    string
		wantCursor int
	}{
		{"backspace mid", "abc", 2, eraseCharBack, "ac", 1},
		{"backspace at zero", "abc", 0, eraseCharBack, "abc", 0},
		{"to start", "abcdef", 3, eraseToStart, "def", 0},
		{"to end", "abcdef", 3, eraseToEnd, "abc", 3},
		{"word back", "one two three", 13, eraseWordBack, "one two ", 8},
		{"word back over spaces", "one two  ", 9, eraseWordBack, "one ", 4},
		{"word back at zero", "abc", 0, eraseWordBack, "abc", 0},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			c := newConnectController(io.Discard, func([]byte) error { return nil })
			c.buf = []byte(tc.seed)
			c.cursor = tc.seedCursor
			c.onInput(inputErase{Kind: tc.kind})
			if string(c.buf) != tc.wantBuf || c.cursor != tc.wantCursor {
				t.Fatalf("buf=%q cursor=%d, want %q / %d", c.buf, c.cursor, tc.wantBuf, tc.wantCursor)
			}
		})
	}
}

func TestConnectControllerHistoryUpDown(t *testing.T) {
	c := newConnectController(io.Discard, func([]byte) error { return nil })
	c.historyOn = true
	c.history = []string{"alpha", "beta"}
	c.writePrompt()
	c.onInput(inputPrintable{Bytes: []byte("wip")})

	c.onInput(inputHistoryUp{})
	if string(c.buf) != "beta" || c.cursor != 4 {
		t.Fatalf("first Up: buf=%q cursor=%d, want beta / 4", c.buf, c.cursor)
	}
	c.onInput(inputHistoryUp{})
	if string(c.buf) != "alpha" || c.cursor != 5 {
		t.Fatalf("second Up: buf=%q cursor=%d, want alpha / 5", c.buf, c.cursor)
	}
	c.onInput(inputHistoryUp{})
	if string(c.buf) != "alpha" {
		t.Fatalf("Up past top changed buf: %q", c.buf)
	}
	c.onInput(inputHistoryDown{})
	if string(c.buf) != "beta" {
		t.Fatalf("Down: buf=%q, want beta", c.buf)
	}
	c.onInput(inputHistoryDown{})
	if string(c.buf) != "wip" || c.cursor != 3 {
		t.Fatalf("Down to current: buf=%q cursor=%d, want wip / 3", c.buf, c.cursor)
	}
	c.onInput(inputHistoryDown{})
	if string(c.buf) != "wip" {
		t.Fatalf("Down past current changed buf: %q", c.buf)
	}
}

func TestConnectControllerHistoryDisabled(t *testing.T) {
	c := newConnectController(io.Discard, func([]byte) error { return nil })
	c.history = []string{"alpha"}
	c.writePrompt()
	c.onInput(inputPrintable{Bytes: []byte("typed")})
	c.onInput(inputHistoryUp{})
	if string(c.buf) != "typed" {
		t.Fatalf("Up without historyOn altered buf: %q", c.buf)
	}
}

func TestConnectControllerInterruptClearsBuffer(t *testing.T) {
	var out bytes.Buffer
	var interrupts int
	c := newConnectController(&out, func([]byte) error { return nil })
	c.sendInterrupt = func() error { interrupts++; return nil }
	c.writePrompt()
	c.onInput(inputPrintable{Bytes: []byte("foo")})

	if exit, _ := c.onInput(inputInterrupt{}); exit {
		t.Fatal("Ctrl-C with buf should not exit")
	}
	if len(c.buf) != 0 || c.cursor != 0 {
		t.Fatalf("after Ctrl-C buf=%q cursor=%d, want empty/0", c.buf, c.cursor)
	}
	if interrupts != 0 {
		t.Fatalf("device interrupts=%d, want 0 (buf-clear path)", interrupts)
	}
	want := promptPrimary + "foo" + "\r\x1b[K" + promptPrimary
	if got := out.String(); got != want {
		t.Fatalf("stdout = %q, want %q", got, want)
	}
}

func TestConnectControllerInterruptEmptyWaitsForDevicePrompt(t *testing.T) {
	var out bytes.Buffer
	var interrupts int
	c := newConnectController(&out, func([]byte) error { return nil })
	c.sendInterrupt = func() error { interrupts++; return nil }
	c.writePrompt()

	startLen := out.Len()
	if exit, _ := c.onInput(inputInterrupt{}); exit {
		t.Fatal("Ctrl-C without buf should not exit on first press")
	}
	if interrupts != 1 {
		t.Fatalf("device interrupts=%d, want 1", interrupts)
	}
	if !c.awaitingPrompt {
		t.Fatal("awaitingPrompt not set after empty-buf Ctrl-C")
	}
	if got := out.String()[startLen:]; got != "" {
		t.Fatalf("client redrew before device prompt arrived: %q", got)
	}

	if exit, _ := c.onDevice(devicePrompt{}); exit {
		t.Fatal("devicePrompt exited")
	}
	if c.awaitingPrompt {
		t.Fatal("awaitingPrompt not cleared on devicePrompt")
	}
	want := "\r\x1b[K" + promptPrimary
	if got := out.String()[startLen:]; got != want {
		t.Fatalf("redraw on devicePrompt = %q, want %q", got, want)
	}
}

func TestConnectControllerDoubleInterruptExits(t *testing.T) {
	c := newConnectController(io.Discard, func([]byte) error { return nil })
	c.sendInterrupt = func() error { return nil }
	now := time.Unix(0, 0)
	c.now = func() time.Time { return now }
	c.writePrompt()

	if exit, _ := c.onInput(inputInterrupt{}); exit {
		t.Fatal("first Ctrl-C exited")
	}
	now = now.Add(500 * time.Millisecond)
	exit, code := c.onInput(inputInterrupt{})
	if !exit || code != 0 {
		t.Fatalf("second Ctrl-C inside window: exit=%v code=%d, want true/0", exit, code)
	}
}

func TestConnectControllerInterruptOutsideWindowDoesNotExit(t *testing.T) {
	c := newConnectController(io.Discard, func([]byte) error { return nil })
	c.sendInterrupt = func() error { return nil }
	now := time.Unix(0, 0)
	c.now = func() time.Time { return now }
	c.writePrompt()

	c.onInput(inputInterrupt{})
	now = now.Add(2 * time.Second)
	if exit, _ := c.onInput(inputInterrupt{}); exit {
		t.Fatal("second Ctrl-C after window exited; should be treated as new first")
	}
}

func TestConnectControllerEOFKeepsBuffer(t *testing.T) {
	c := newConnectController(io.Discard, func([]byte) error { return nil })
	c.writePrompt()
	c.onInput(inputPrintable{Bytes: []byte("foo")})

	if exit, _ := c.onInput(inputEOF{}); exit {
		t.Fatal("Ctrl-D with buf should not exit")
	}
	if string(c.buf) != "foo" {
		t.Fatalf("Ctrl-D mutated buf: %q", c.buf)
	}
}

func TestConnectControllerEOFEmptyExits(t *testing.T) {
	c := newConnectController(io.Discard, func([]byte) error { return nil })
	c.writePrompt()
	exit, code := c.onInput(inputEOF{})
	if !exit || code != 0 {
		t.Fatalf("Ctrl-D on empty: exit=%v code=%d, want true/0", exit, code)
	}
}

func TestConnectControllerIdleFireRedraws(t *testing.T) {
	var out bytes.Buffer
	c := newConnectController(&out, func([]byte) error { return nil })
	c.writePrompt()
	c.onInput(inputPrintable{Bytes: []byte("foo")})
	c.onDevice(deviceBytes{Bytes: []byte("notice\r\n")})
	c.onIdleFire()

	want := promptPrimary + "foo" + "\r\x1b[K" + "notice\r\n" + promptPrimary + "foo"
	if got := out.String(); got != want {
		t.Fatalf("stdout = %q, want %q", got, want)
	}
}
