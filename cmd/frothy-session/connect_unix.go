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

// runConnectInteractive owns the main loop and the redraw. Device output
// arriving while a partial input line is buffered is held back until the
// user stops typing for idleFlushDelay, then the input line is erased,
// the device output is printed, and the prompt + buffer are redrawn.
func runConnectInteractive(dev *serialDevice, stdin io.Reader, stdout io.Writer) int {
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

	var buf []byte
	var pending []byte
	idle := time.NewTimer(time.Hour)
	idle.Stop()
	idleArmed := false

	writePrompt := func() {
		_, _ = io.WriteString(stdout, promptPrimary)
		if len(buf) > 0 {
			_, _ = stdout.Write(buf)
		}
	}
	flushPending := func() {
		if len(pending) == 0 {
			idleArmed = false
			return
		}
		if len(buf) > 0 {
			_, _ = io.WriteString(stdout, "\r\x1b[K")
		}
		_, _ = stdout.Write(pending)
		pending = pending[:0]
		idleArmed = false
		if len(buf) > 0 {
			writePrompt()
		}
	}
	armIdle := func() {
		if !idleArmed {
			idle.Reset(idleFlushDelay)
			idleArmed = true
		}
	}
	resetIdle := func() {
		if idleArmed {
			if !idle.Stop() {
				select {
				case <-idle.C:
				default:
				}
			}
			idle.Reset(idleFlushDelay)
		}
	}

	writePrompt()

	for {
		select {
		case ev, ok := <-inputs:
			if !ok {
				flushPending()
				return 0
			}
			resetIdle()
			switch e := ev.(type) {
			case inputPrintable:
				buf = append(buf, e.Bytes...)
				_, _ = stdout.Write(e.Bytes)
			case inputSubmit:
				_, _ = io.WriteString(stdout, "\n")
				line := string(buf) + "\n"
				_ = dev.writeBytes([]byte(line))
				buf = buf[:0]
				pending = pending[:0]
				idleArmed = false
				writePrompt()
			case inputInterrupt:
				// Decision 7 (Ctrl-C rules) is not wired here yet.
			case inputEOF:
				flushPending()
				return 0
			case inputError:
				flushPending()
				return 1
			}
		case ev, ok := <-devices:
			if !ok {
				flushPending()
				return 0
			}
			switch e := ev.(type) {
			case deviceBytes:
				if len(buf) == 0 {
					_, _ = stdout.Write(e.Bytes)
				} else {
					pending = append(pending, e.Bytes...)
					armIdle()
				}
			case devicePrompt:
				// Informational; the renderer prints "> " as part of the
				// preceding deviceBytes.
			case deviceError:
				flushPending()
				return 1
			}
		case <-idle.C:
			idleArmed = false
			flushPending()
		}
	}
}
