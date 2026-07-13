//go:build darwin || linux

package main

import (
	"fmt"
	"io"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"
)

// idleFlushDelay is the quiet window after the last input byte before
// buffered device output is flushed and the input line is redrawn.
const idleFlushDelay = 50 * time.Millisecond

// doubleInterruptWindow is the window for decision 7's second Ctrl-C
// to exit the client.
const doubleInterruptWindow = time.Second

// terminalLineBreak is emitted by the host-side editor while stdin is in raw
// mode. A bare "\n" would move down without returning to column 0.
const terminalLineBreak = "\r\n"

const plainLineBreak = "\n"

func runConnect() int {
	return runConnectCommand(os.Args[1:], os.Stdin, os.Stdout, os.Stderr, defaultPortLister, defaultConnectDeviceFactory, runConnectInteractiveWithCleanup)
}

func defaultConnectDeviceFactory(port string, baud int) (*serialDevice, func(), error) {
	dev, err := openSerial(port, baud)
	if err != nil {
		return nil, nil, err
	}
	return dev, func() { dev.close() }, nil
}

// runConnectInteractiveWithCleanup is the production entry: it installs a
// SIGTERM/SIGHUP/SIGINT handler, puts stdin in raw mode if it points at a
// tty, turns on bracketed paste, and runs the interactive loop. All of that
// is undone on every exit path through a single sync.Once cleanup. Raw mode
// stops Ctrl-C on stdin from generating SIGINT (decision 7 owns 0x03);
// SIGINT here covers external `kill -INT`.
func runConnectInteractiveWithCleanup(dev *serialDevice, stdin io.Reader, stdout io.Writer, hist connectHistory) int {
	sigCh := make(chan os.Signal, 4)
	signal.Notify(sigCh, syscall.SIGTERM, syscall.SIGHUP, syscall.SIGINT)
	defer signal.Stop(sigCh)
	return runConnectInteractiveTermios(dev, stdin, stdout, hist, sigCh)
}

// runConnectInteractiveTermios is the seam tests reach for: the production
// wrapper is signal.Notify + this; tests inject a plain channel and send
// signal values directly without poking the real process.
func runConnectInteractiveTermios(dev *serialDevice, stdin io.Reader, stdout io.Writer, hist connectHistory, sigCh <-chan os.Signal) int {
	var state *terminalState
	if f, ok := stdin.(*os.File); ok {
		if s, err := enterRawMode(f); err == nil {
			state = s
		}
	}
	terminal := state != nil && writerIsTerminal(stdout)
	if terminal {
		_, _ = io.WriteString(stdout, bracketedPasteOn)
	}

	var cleanupOnce sync.Once
	cleanup := func() {
		cleanupOnce.Do(func() {
			if terminal {
				_, _ = io.WriteString(stdout, bracketedPasteOff)
			}
			if state != nil {
				_ = state.restore()
			}
		})
	}
	defer cleanup()

	shutdown := make(chan int, 1)
	sigDone := make(chan struct{})
	defer close(sigDone)
	go func() {
		select {
		case sig := <-sigCh:
			shutdown <- signalExitCode(sig)
		case <-sigDone:
		}
	}()

	return runConnectInteractiveBase(dev, stdin, stdout, hist, terminal, shutdown)
}

// signalExitCode follows the conventional 128+signum mapping. Decision 9
// names SIGTERM (143) and SIGINT (130); SIGHUP (129) rounds out the
// posted signals.
func signalExitCode(sig os.Signal) int {
	if s, ok := sig.(syscall.Signal); ok {
		return 128 + int(s)
	}
	return 1
}

func runConnectInteractive(dev *serialDevice, stdin io.Reader, stdout io.Writer, hist connectHistory) int {
	return runConnectInteractiveBase(dev, stdin, stdout, hist, true, nil)
}

func runConnectInteractiveBase(dev *serialDevice, stdin io.Reader, stdout io.Writer, hist connectHistory, terminal bool, shutdown <-chan int) int {
	inputs := make(chan inputEvent, 64)
	devices := make(chan deviceEvent, 64)
	stop := make(chan struct{})
	stopped := false
	closeStop := func() {
		if !stopped {
			stopped = true
			close(stop)
		}
	}
	defer closeStop()

	go runInputReader(stdin, inputs, stop)
	go runSerialEventPump(dev.readCh, dev.errCh, devices, stop)

	c := newConnectController(stdout, dev.writeBytes)
	c.terminal = terminal
	c.sendInterrupt = dev.sendInterrupt
	if hist.enabled {
		c.historyOn = true
		c.history = append([]string(nil), hist.initial...)
	}
	code := runConnectLoop(c, inputs, devices, shutdown)
	if hist.enabled && hist.path != "" {
		_ = saveHistory(hist.path, c.history)
	}
	return code
}

type connectController struct {
	out                    io.Writer
	sendLine               func([]byte) error
	sendInterrupt          func() error
	now                    func() time.Time
	terminal               bool
	historyOn              bool
	history                []string
	histIdx                int // -1 = editing current line; >=0 = replayed entry index
	savedLine              []byte
	savedCur               int
	buf                    []byte
	cursor                 int
	form                   sourceFormState
	pending                []byte
	idleArmed              bool
	lastInterruptAt        time.Time
	hasInterrupt           bool
	awaitingPrompt         bool
	redrawOnPrompt         bool
	deviceTextSincePrompt  bool
	expectedEchoes         []expectedDeviceEcho
	resolvedExpectedEchoes int
}

func newConnectController(out io.Writer, sendLine func([]byte) error) *connectController {
	return &connectController{out: out, sendLine: sendLine, now: time.Now, terminal: true, histIdx: -1}
}

type expectedDeviceEcho struct {
	bytes      []byte
	pos        int
	sawNewline bool
	trailingCR bool
}

type echoConsumeAction int

const (
	echoConsumed echoConsumeAction = iota
	echoConsumedAndDone
	echoDoneBeforeByte
	echoMismatch
)

func (c *connectController) writePrompt() {
	prompt := promptPrimary
	if c.form.hasPending() {
		prompt = promptContinuation
	}
	_, _ = io.WriteString(c.out, prompt)
	if len(c.buf) > 0 {
		_, _ = c.out.Write(c.buf)
	}
	if back := len(c.buf) - c.cursor; c.terminal && back > 0 {
		fmt.Fprintf(c.out, "\x1b[%dD", back)
	}
	c.deviceTextSincePrompt = false
}

func (c *connectController) redrawLine() {
	if c.terminal {
		_, _ = io.WriteString(c.out, "\r\x1b[K")
	}
	c.writePrompt()
}

func (c *connectController) flushPending() {
	if len(c.pending) == 0 {
		c.idleArmed = false
		return
	}
	if c.terminal && len(c.buf) > 0 {
		_, _ = io.WriteString(c.out, "\r\x1b[K")
	}
	_, _ = c.out.Write(c.pending)
	c.pending = c.pending[:0]
	c.idleArmed = false
	if len(c.buf) > 0 {
		c.writePrompt()
	}
}

func (c *connectController) lineBreak() string {
	if c.terminal {
		return terminalLineBreak
	}
	return plainLineBreak
}

func (c *connectController) recordExpectedEcho(sent []byte) {
	line := append([]byte(nil), sent...)
	for len(line) > 0 && (line[len(line)-1] == '\n' || line[len(line)-1] == '\r') {
		line = line[:len(line)-1]
	}
	if len(line) == 0 {
		return
	}
	c.expectedEchoes = append(c.expectedEchoes, expectedDeviceEcho{bytes: line})
}

func (e *expectedDeviceEcho) consume(b byte) echoConsumeAction {
	if e.pos < len(e.bytes) {
		if b != e.bytes[e.pos] {
			return echoMismatch
		}
		e.pos++
		return echoConsumed
	}
	if e.sawNewline {
		if e.trailingCR && b == '\n' {
			e.trailingCR = false
			return echoConsumedAndDone
		}
		return echoDoneBeforeByte
	}
	switch b {
	case '\r':
		e.sawNewline = true
		e.trailingCR = true
		return echoConsumed
	case '\n':
		e.sawNewline = true
		return echoConsumedAndDone
	default:
		return echoMismatch
	}
}

func (c *connectController) finishExpectedEcho() {
	if len(c.expectedEchoes) == 0 {
		return
	}
	c.expectedEchoes = c.expectedEchoes[1:]
	c.resolvedExpectedEchoes++
}

func (c *connectController) finishExpectedEchoForPrompt() {
	if c.resolvedExpectedEchoes > 0 {
		c.resolvedExpectedEchoes--
		return
	}
	if len(c.expectedEchoes) > 0 {
		c.expectedEchoes = c.expectedEchoes[1:]
	}
}

func (c *connectController) swallowExpectedEcho(b []byte) []byte {
	if len(c.expectedEchoes) == 0 || len(b) == 0 {
		return b
	}
	var out []byte
	for i := 0; i < len(b); {
		if len(c.expectedEchoes) == 0 {
			out = append(out, b[i:]...)
			break
		}
		switch c.expectedEchoes[0].consume(b[i]) {
		case echoConsumed:
			i++
		case echoConsumedAndDone:
			i++
			c.finishExpectedEcho()
		case echoDoneBeforeByte:
			c.finishExpectedEcho()
		case echoMismatch:
			c.finishExpectedEcho()
			out = append(out, b[i:]...)
			i = len(b)
		}
	}
	return out
}

func (c *connectController) onInput(ev inputEvent) (exit bool, code int) {
	switch e := ev.(type) {
	case inputPrintable:
		if c.cursor == len(c.buf) {
			c.buf = append(c.buf, e.Bytes...)
			c.cursor = len(c.buf)
			_, _ = c.out.Write(e.Bytes)
		} else {
			tail := append([]byte(nil), c.buf[c.cursor:]...)
			c.buf = append(c.buf[:c.cursor], e.Bytes...)
			c.buf = append(c.buf, tail...)
			c.cursor += len(e.Bytes)
			c.redrawLine()
		}
	case inputCursorMove:
		c.moveCursor(e.Dir)
	case inputErase:
		c.erase(e.Kind)
	case inputHistoryUp:
		c.historyUp()
	case inputHistoryDown:
		c.historyDown()
	case inputSubmit:
		_, _ = io.WriteString(c.out, c.lineBreak())
		if len(c.pending) > 0 {
			_, _ = c.out.Write(c.pending)
			c.pending = c.pending[:0]
		}
		c.idleArmed = false
		line := string(c.buf)
		c.buf = c.buf[:0]
		c.cursor = 0
		c.histIdx = -1
		c.savedLine = c.savedLine[:0]
		c.savedCur = 0
		if source, complete := c.form.appendLine(line); complete {
			if c.historyOn && !strings.ContainsAny(source, "\r\n") {
				c.history = appendHistory(c.history, source)
			}
			sent := []byte(wireRequest(source) + "\n")
			if err := c.sendLine(sent); err == nil {
				c.recordExpectedEcho(sent)
				c.awaitingPrompt = true
				c.redrawOnPrompt = false
				return false, 0
			}
		}
		c.writePrompt()
	case inputInterrupt:
		return c.onInterrupt()
	case inputEOF:
		if len(c.buf) > 0 || c.form.hasPending() {
			return false, 0
		}
		c.flushPending()
		return true, 0
	case inputError:
		_ = e
		c.flushPending()
		return true, 1
	}
	return false, 0
}

// Decision 7: second Ctrl-C inside the window exits; first Ctrl-C with a
// buffered line clears it without poking the device; first Ctrl-C with an
// empty line forwards 0x03 to the device.
func (c *connectController) onInterrupt() (exit bool, code int) {
	now := c.now()
	if c.hasInterrupt && now.Sub(c.lastInterruptAt) <= doubleInterruptWindow {
		c.flushPending()
		return true, 0
	}
	c.hasInterrupt = true
	c.lastInterruptAt = now
	if len(c.buf) > 0 || c.form.hasPending() {
		c.buf = c.buf[:0]
		c.cursor = 0
		c.histIdx = -1
		c.savedLine = c.savedLine[:0]
		c.savedCur = 0
		c.form.reset()
		c.redrawLine()
		return false, 0
	}
	if c.sendInterrupt != nil {
		_ = c.sendInterrupt()
		c.awaitingPrompt = true
		c.redrawOnPrompt = true
		return false, 0
	}
	c.redrawLine()
	return false, 0
}

func (c *connectController) moveCursor(d cursorDir) {
	switch d {
	case cursorHome:
		c.cursor = 0
	case cursorEnd:
		c.cursor = len(c.buf)
	case cursorLeft:
		if c.cursor > 0 {
			c.cursor--
		}
	case cursorRight:
		if c.cursor < len(c.buf) {
			c.cursor++
		}
	}
	c.redrawLine()
}

func (c *connectController) erase(k eraseKind) {
	switch k {
	case eraseCharBack:
		if c.cursor == 0 {
			return
		}
		c.buf = append(c.buf[:c.cursor-1], c.buf[c.cursor:]...)
		c.cursor--
	case eraseWordBack:
		end := c.cursor
		i := end
		for i > 0 && c.buf[i-1] == ' ' {
			i--
		}
		for i > 0 && c.buf[i-1] != ' ' {
			i--
		}
		if i == end {
			return
		}
		c.buf = append(c.buf[:i], c.buf[end:]...)
		c.cursor = i
	case eraseToStart:
		if c.cursor == 0 {
			return
		}
		c.buf = append(c.buf[:0], c.buf[c.cursor:]...)
		c.cursor = 0
	case eraseToEnd:
		if c.cursor == len(c.buf) {
			return
		}
		c.buf = c.buf[:c.cursor]
	}
	c.redrawLine()
}

func (c *connectController) historyUp() {
	if !c.historyOn || len(c.history) == 0 {
		return
	}
	switch {
	case c.histIdx == -1:
		c.savedLine = append(c.savedLine[:0], c.buf...)
		c.savedCur = c.cursor
		c.histIdx = len(c.history) - 1
	case c.histIdx > 0:
		c.histIdx--
	default:
		return
	}
	c.buf = append(c.buf[:0], c.history[c.histIdx]...)
	c.cursor = len(c.buf)
	c.redrawLine()
}

func (c *connectController) historyDown() {
	if !c.historyOn || c.histIdx == -1 {
		return
	}
	if c.histIdx < len(c.history)-1 {
		c.histIdx++
		c.buf = append(c.buf[:0], c.history[c.histIdx]...)
		c.cursor = len(c.buf)
	} else {
		c.histIdx = -1
		c.buf = append(c.buf[:0], c.savedLine...)
		c.cursor = c.savedCur
	}
	c.redrawLine()
}

func (c *connectController) writeDeviceText(b []byte) {
	b = c.swallowExpectedEcho(b)
	if len(b) == 0 {
		return
	}
	c.deviceTextSincePrompt = true
	if len(c.buf) == 0 {
		_, _ = c.out.Write(b)
		return
	}
	c.pending = append(c.pending, b...)
	c.idleArmed = true
}

func (c *connectController) onDevice(ev deviceEvent) (exit bool, code int) {
	switch e := ev.(type) {
	case deviceBytes:
		c.writeDeviceText(e.Bytes)
	case deviceResetStart:
		c.writeDeviceText([]byte("-- device reset detected; waiting for prompt --" + c.lineBreak()))
	case deviceResetEnd:
		c.writeDeviceText([]byte("-- prompt restored --" + c.lineBreak()))
	case devicePrompt:
		c.finishExpectedEchoForPrompt()
		if c.awaitingPrompt {
			c.awaitingPrompt = false
			if c.redrawOnPrompt {
				c.redrawLine()
			} else {
				c.writePrompt()
			}
			c.redrawOnPrompt = false
		} else if c.deviceTextSincePrompt && len(c.buf) == 0 {
			c.writePrompt()
		}
	case deviceError:
		_ = e
		c.flushPending()
		return true, 1
	}
	return false, 0
}

func (c *connectController) onIdleFire() {
	c.idleArmed = false
	c.flushPending()
}

func runConnectLoop(c *connectController, inputs <-chan inputEvent, devices <-chan deviceEvent, shutdown <-chan int) int {
	idle := time.NewTimer(time.Hour)
	idle.Stop()
	timerArmed := false
	resetTimer := func() {
		if timerArmed {
			if !idle.Stop() {
				select {
				case <-idle.C:
				default:
				}
			}
			idle.Reset(idleFlushDelay)
		}
	}
	syncTimer := func() {
		if c.idleArmed && !timerArmed {
			idle.Reset(idleFlushDelay)
			timerArmed = true
		} else if !c.idleArmed && timerArmed {
			if !idle.Stop() {
				select {
				case <-idle.C:
				default:
				}
			}
			timerArmed = false
		}
	}

	c.writePrompt()

	for {
		select {
		case ev, ok := <-inputs:
			if !ok {
				c.flushPending()
				return 0
			}
			resetTimer()
			if exit, code := c.onInput(ev); exit {
				return code
			}
			syncTimer()
		case ev, ok := <-devices:
			if !ok {
				c.flushPending()
				return 0
			}
			if exit, code := c.onDevice(ev); exit {
				return code
			}
			syncTimer()
		case <-idle.C:
			timerArmed = false
			c.onIdleFire()
		case code := <-shutdown:
			c.flushPending()
			return code
		}
	}
}
