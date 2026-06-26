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

	var lineBuf []byte
	inReset := false
	promptLineStart := true
	heldPromptGT := false

	emitPrompt := func() bool {
		if inReset {
			inReset = false
			if !emit(deviceResetEnd{}) {
				return false
			}
		}
		promptLineStart = false
		return emit(devicePrompt{})
	}

	emitBytes := func(chunk []byte) bool {
		if len(chunk) == 0 {
			return true
		}

		// Scan completed lines for the banner. A reset session opens at
		// the first matching line and closes at the next prompt. The
		// split index is the byte right after the \n that closed the
		// matched line: the reset notice has to land before any further
		// device bytes reach the user.
		splitAt := -1
		for i, bb := range chunk {
			switch bb {
			case '\n':
				if !inReset && isResetBannerLine(string(lineBuf)) {
					inReset = true
					splitAt = i + 1
				}
				lineBuf = lineBuf[:0]
				promptLineStart = true
			case '\r':
				promptLineStart = true
			default:
				promptLineStart = false
				if len(lineBuf) < resetLineCap {
					lineBuf = append(lineBuf, bb)
				}
			}
		}

		if splitAt >= 0 {
			if splitAt > 0 {
				if !emit(deviceBytes{Bytes: chunk[:splitAt]}) {
					return false
				}
			}
			if !emit(deviceResetStart{}) {
				return false
			}
			if splitAt < len(chunk) {
				if !emit(deviceBytes{Bytes: chunk[splitAt:]}) {
					return false
				}
			}
			return true
		}
		return emit(deviceBytes{Bytes: chunk})
	}

	flushHeldPromptGT := func() bool {
		if !heldPromptGT {
			return true
		}
		heldPromptGT = false
		return emitBytes([]byte{'>'})
	}

	processChunk := func(chunk []byte) bool {
		if heldPromptGT {
			heldPromptGT = false
			if len(chunk) > 0 && chunk[0] == ' ' {
				if !emitPrompt() {
					return false
				}
				chunk = chunk[1:]
			} else {
				prefixed := make([]byte, 0, len(chunk)+1)
				prefixed = append(prefixed, '>')
				prefixed = append(prefixed, chunk...)
				chunk = prefixed
			}
		}

		if len(chunk) == 0 {
			return true
		}

		for len(chunk) > 0 {
			promptAt := -1
			for i := 0; i+1 < len(chunk); i++ {
				if chunk[i] != '>' || chunk[i+1] != ' ' {
					continue
				}
				if i == 0 {
					if promptLineStart {
						promptAt = i
						break
					}
					continue
				}
				prev := chunk[i-1]
				if prev == '\n' || prev == '\r' {
					promptAt = i
					break
				}
			}
			if promptAt >= 0 {
				if !emitBytes(chunk[:promptAt]) {
					return false
				}
				if !emitPrompt() {
					return false
				}
				chunk = chunk[promptAt+2:]
				continue
			}

			if chunk[len(chunk)-1] == '>' {
				atPromptStart := len(chunk) == 1 && promptLineStart
				if len(chunk) > 1 {
					prev := chunk[len(chunk)-2]
					atPromptStart = prev == '\n' || prev == '\r'
				}
				if atPromptStart {
					heldPromptGT = true
					chunk = chunk[:len(chunk)-1]
				}
			}

			return emitBytes(chunk)
		}
		return true
	}

	for {
		var b byte
		select {
		case bb, ok := <-readCh:
			if !ok {
				_ = flushHeldPromptGT()
				return
			}
			b = bb
		case err := <-errCh:
			if !flushHeldPromptGT() {
				return
			}
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

		if !processChunk(chunk) {
			return
		}
	}
}
