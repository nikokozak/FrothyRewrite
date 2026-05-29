//go:build darwin || linux

package main

import (
	"fmt"
	"io"
	"os"
	"time"
)

// idleFlushDelay is the quiet window after the last input byte before
// buffered device output is flushed and the input line is redrawn.
const idleFlushDelay = 50 * time.Millisecond

// doubleInterruptWindow is the window for decision 7's second Ctrl-C
// to exit the client.
const doubleInterruptWindow = time.Second

func runConnect() int {
	return runConnectCommand(os.Args[1:], os.Stdin, os.Stdout, os.Stderr, defaultPortLister, defaultConnectDeviceFactory, runConnectInteractive)
}

func defaultConnectDeviceFactory(port string, baud int) (*serialDevice, func(), error) {
	dev, err := openSerial(port, baud)
	if err != nil {
		return nil, nil, err
	}
	return dev, func() { dev.close() }, nil
}

func runConnectInteractive(dev *serialDevice, stdin io.Reader, stdout io.Writer, hist connectHistory) int {
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
	c.sendInterrupt = dev.sendInterrupt
	if hist.enabled {
		c.historyOn = true
		c.history = append([]string(nil), hist.initial...)
	}
	code := runConnectLoop(c, inputs, devices)
	if hist.enabled && hist.path != "" {
		_ = saveHistory(hist.path, c.history)
	}
	return code
}

type connectController struct {
	out             io.Writer
	sendLine        func([]byte) error
	sendInterrupt   func() error
	now             func() time.Time
	historyOn       bool
	history         []string
	histIdx         int // -1 = editing current line; >=0 = replayed entry index
	savedLine       []byte
	savedCur        int
	buf             []byte
	cursor          int
	pending         []byte
	idleArmed       bool
	lastInterruptAt time.Time
	hasInterrupt    bool
}

func newConnectController(out io.Writer, sendLine func([]byte) error) *connectController {
	return &connectController{out: out, sendLine: sendLine, now: time.Now, histIdx: -1}
}

func (c *connectController) writePrompt() {
	_, _ = io.WriteString(c.out, promptPrimary)
	if len(c.buf) > 0 {
		_, _ = c.out.Write(c.buf)
	}
	if back := len(c.buf) - c.cursor; back > 0 {
		fmt.Fprintf(c.out, "\x1b[%dD", back)
	}
}

func (c *connectController) redrawLine() {
	_, _ = io.WriteString(c.out, "\r\x1b[K")
	c.writePrompt()
}

func (c *connectController) flushPending() {
	if len(c.pending) == 0 {
		c.idleArmed = false
		return
	}
	if len(c.buf) > 0 {
		_, _ = io.WriteString(c.out, "\r\x1b[K")
	}
	_, _ = c.out.Write(c.pending)
	c.pending = c.pending[:0]
	c.idleArmed = false
	if len(c.buf) > 0 {
		c.writePrompt()
	}
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
		_, _ = io.WriteString(c.out, "\n")
		if len(c.pending) > 0 {
			_, _ = c.out.Write(c.pending)
			c.pending = c.pending[:0]
		}
		c.idleArmed = false
		line := string(c.buf)
		if c.historyOn {
			c.history = appendHistory(c.history, line)
		}
		_ = c.sendLine([]byte(line + "\n"))
		c.buf = c.buf[:0]
		c.cursor = 0
		c.histIdx = -1
		c.savedLine = c.savedLine[:0]
		c.savedCur = 0
		c.writePrompt()
	case inputInterrupt:
		return c.onInterrupt()
	case inputEOF:
		if len(c.buf) > 0 {
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
	if len(c.buf) > 0 {
		c.buf = c.buf[:0]
		c.cursor = 0
		c.histIdx = -1
		c.savedLine = c.savedLine[:0]
		c.savedCur = 0
		c.redrawLine()
		return false, 0
	}
	if c.sendInterrupt != nil {
		_ = c.sendInterrupt()
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

func (c *connectController) onDevice(ev deviceEvent) (exit bool, code int) {
	switch e := ev.(type) {
	case deviceBytes:
		if len(c.buf) == 0 {
			_, _ = c.out.Write(e.Bytes)
		} else {
			c.pending = append(c.pending, e.Bytes...)
			c.idleArmed = true
		}
	case devicePrompt:
		// Informational; the renderer prints "> " as part of deviceBytes.
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

func runConnectLoop(c *connectController, inputs <-chan inputEvent, devices <-chan deviceEvent) int {
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
		}
	}
}
