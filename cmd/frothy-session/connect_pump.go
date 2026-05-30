//go:build darwin || linux

package main

// Serial event pump. One goroutine owns serialDevice.readCh/errCh and
// pushes typed events at the main loop. Order is preserved byte-for-byte;
// the per-batch drain just coalesces a chunk so the main loop sees fewer
// events during a burst.

type deviceEvent interface{ deviceEventMarker() }

type deviceBytes struct{ Bytes []byte }
type devicePrompt struct{}
type deviceResetStart struct{}
type deviceResetEnd struct{}
type deviceError struct{ Err error }

func (deviceBytes) deviceEventMarker()      {}
func (devicePrompt) deviceEventMarker()     {}
func (deviceResetStart) deviceEventMarker() {}
func (deviceResetEnd) deviceEventMarker()   {}
func (deviceError) deviceEventMarker()      {}

// resetLineCap bounds the per-line buffer used for banner-prefix matching.
// Banner prefixes are short ("ets ", "entry 0x"); a few dozen bytes is
// plenty, and the cap stops a single runaway line from growing forever.
const resetLineCap = 64

func runSerialEventPump(readCh <-chan byte, errCh <-chan error, events chan<- deviceEvent, stop <-chan struct{}) {
	defer close(events)

	emit := func(e deviceEvent) bool {
		select {
		case events <- e:
			return true
		case <-stop:
			return false
		}
	}

	var prev byte
	var lineBuf []byte
	inReset := false
	for {
		var b byte
		select {
		case bb, ok := <-readCh:
			if !ok {
				return
			}
			b = bb
		case err := <-errCh:
			_ = emit(deviceError{Err: err})
			return
		case <-stop:
			return
		}

		chunk := []byte{b}
	drain:
		for {
			select {
			case b2, ok := <-readCh:
				if !ok {
					break drain
				}
				chunk = append(chunk, b2)
			default:
				break drain
			}
		}

		// Detect "> " prompt tail, including the case where '>' was the
		// last byte of the previous chunk and ' ' is the first of this one.
		n := len(chunk)
		last := chunk[n-1]
		promptTail := last == ' ' && ((n >= 2 && chunk[n-2] == '>') || (n == 1 && prev == '>'))
		prev = last

		// Scan completed lines for the banner. A reset session opens at
		// the first matching line and closes at the next prompt. The
		// split index is the byte right after the \n that closed the
		// matched line: the reset notice has to land before any further
		// device bytes (especially the recovery `> `) reach the user.
		splitAt := -1
		for i, bb := range chunk {
			switch bb {
			case '\n':
				if !inReset && isResetBannerLine(string(lineBuf)) {
					inReset = true
					splitAt = i + 1
				}
				lineBuf = lineBuf[:0]
			case '\r':
			default:
				if len(lineBuf) < resetLineCap {
					lineBuf = append(lineBuf, bb)
				}
			}
		}

		if splitAt >= 0 {
			if splitAt > 0 {
				if !emit(deviceBytes{Bytes: chunk[:splitAt]}) {
					return
				}
			}
			if !emit(deviceResetStart{}) {
				return
			}
			if splitAt < len(chunk) {
				if !emit(deviceBytes{Bytes: chunk[splitAt:]}) {
					return
				}
			}
		} else {
			if !emit(deviceBytes{Bytes: chunk}) {
				return
			}
		}
		if promptTail {
			if inReset {
				inReset = false
				if !emit(deviceResetEnd{}) {
					return
				}
			}
			if !emit(devicePrompt{}) {
				return
			}
		}
	}
}
