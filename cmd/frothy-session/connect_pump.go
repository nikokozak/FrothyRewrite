//go:build darwin || linux

package main

// Serial event pump. One goroutine owns serialDevice.readCh/errCh and
// pushes typed events at the main loop. Order is preserved byte-for-byte;
// the per-batch drain just coalesces a chunk so the main loop sees fewer
// events during a burst.

type deviceEvent interface{ deviceEventMarker() }

type deviceBytes struct{ Bytes []byte }
type devicePrompt struct{}
type deviceError struct{ Err error }

func (deviceBytes) deviceEventMarker()  {}
func (devicePrompt) deviceEventMarker() {}
func (deviceError) deviceEventMarker()  {}

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

		if !emit(deviceBytes{Bytes: chunk}) {
			return
		}
		if promptTail {
			if !emit(devicePrompt{}) {
				return
			}
		}
	}
}
