//go:build darwin || linux

package main

import (
	"io"
	"os"
	"time"
)

// idleFlushDelay is the quiet window after the last input byte before
// buffered device output is flushed and the input line is redrawn.
const idleFlushDelay = 50 * time.Millisecond

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

	entries := append([]string(nil), hist.initial...)
	c := newConnectController(stdout, dev.writeBytes)
	if hist.enabled {
		c.addHistory = func(line string) {
			entries = appendHistory(entries, line)
		}
	}
	code := runConnectLoop(c, inputs, devices)
	if hist.enabled && hist.path != "" {
		_ = saveHistory(hist.path, entries)
	}
	return code
}

type connectController struct {
	out        io.Writer
	sendLine   func([]byte) error
	addHistory func(string)
	buf        []byte
	pending    []byte
	idleArmed  bool
}

func newConnectController(out io.Writer, sendLine func([]byte) error) *connectController {
	return &connectController{out: out, sendLine: sendLine}
}

func (c *connectController) writePrompt() {
	_, _ = io.WriteString(c.out, promptPrimary)
	if len(c.buf) > 0 {
		_, _ = c.out.Write(c.buf)
	}
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
		c.buf = append(c.buf, e.Bytes...)
		_, _ = c.out.Write(e.Bytes)
	case inputSubmit:
		_, _ = io.WriteString(c.out, "\n")
		if len(c.pending) > 0 {
			_, _ = c.out.Write(c.pending)
			c.pending = c.pending[:0]
		}
		c.idleArmed = false
		line := string(c.buf)
		if c.addHistory != nil {
			c.addHistory(line)
		}
		_ = c.sendLine([]byte(line + "\n"))
		c.buf = c.buf[:0]
		c.writePrompt()
	case inputInterrupt:
		// Decision 7 (Ctrl-C rules) is not wired here yet.
	case inputEOF:
		c.flushPending()
		return true, 0
	case inputError:
		_ = e
		c.flushPending()
		return true, 1
	}
	return false, 0
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
