//go:build darwin || linux

package main

import (
	"errors"
	"io"
)

// Stdin reader. Parses every key listed under decision 2 of the spec
// (printables, line-editing controls, arrow keys, bracketed paste) into
// the inputEvent stream the main loop consumes.
//
// Escape parsing is stateful: the CSI buffer carries across reads, so
// "ESC" in one Read and "[A" in the next still emits one inputHistoryUp.
// Unknown CSIs are consumed up to the final byte and dropped; a bare ESC
// followed by a non-'[' byte drops the ESC and reprocesses that byte
// normally.

type inputEvent interface{ inputEventMarker() }

type cursorDir int

const (
	cursorHome cursorDir = iota
	cursorEnd
	cursorLeft
	cursorRight
)

type eraseKind int

const (
	eraseCharBack eraseKind = iota
	eraseWordBack
	eraseToStart
	eraseToEnd
)

type inputPrintable struct{ Bytes []byte }
type inputCursorMove struct{ Dir cursorDir }
type inputErase struct{ Kind eraseKind }
type inputHistoryUp struct{}
type inputHistoryDown struct{}
type inputPasteStart struct{}
type inputPasteEnd struct{}
type inputSubmit struct{}
type inputInterrupt struct{}
type inputEOF struct{}
type inputError struct{ Err error }

func (inputPrintable) inputEventMarker()   {}
func (inputCursorMove) inputEventMarker()  {}
func (inputErase) inputEventMarker()       {}
func (inputHistoryUp) inputEventMarker()   {}
func (inputHistoryDown) inputEventMarker() {}
func (inputPasteStart) inputEventMarker()  {}
func (inputPasteEnd) inputEventMarker()    {}
func (inputSubmit) inputEventMarker()      {}
func (inputInterrupt) inputEventMarker()   {}
func (inputEOF) inputEventMarker()         {}
func (inputError) inputEventMarker()       {}

// Cap on CSI buffer length. ESC [ 200 ~ is 6 bytes; anything longer is
// a foreign sequence we don't recognize, so drop it.
const escBufMax = 16

type inputParser struct {
	escBuf  []byte
	pending []byte
}

func newInputParser() *inputParser {
	return &inputParser{}
}

// parse feeds bytes into the parser. emit returns false when the caller
// wants the reader to stop (channel send raced with shutdown).
func (p *inputParser) parse(in []byte, emit func(inputEvent) bool) bool {
	for _, b := range in {
		if !p.handleByte(b, emit) {
			return false
		}
	}
	return true
}

// flushPrintable emits any buffered printable bytes as one event.
func (p *inputParser) flushPrintable(emit func(inputEvent) bool) bool {
	if len(p.pending) == 0 {
		return true
	}
	bs := append([]byte(nil), p.pending...)
	p.pending = p.pending[:0]
	return emit(inputPrintable{Bytes: bs})
}

func (p *inputParser) handleByte(b byte, emit func(inputEvent) bool) bool {
	if len(p.escBuf) > 0 {
		return p.handleEscByte(b, emit)
	}
	if b == 0x1b {
		if !p.flushPrintable(emit) {
			return false
		}
		p.escBuf = append(p.escBuf, b)
		return true
	}
	return p.handleRegularByte(b, emit)
}

func (p *inputParser) handleRegularByte(b byte, emit func(inputEvent) bool) bool {
	emitWithFlush := func(e inputEvent) bool {
		if !p.flushPrintable(emit) {
			return false
		}
		return emit(e)
	}
	switch b {
	case 0x03:
		return emitWithFlush(inputInterrupt{})
	case 0x04:
		return emitWithFlush(inputEOF{})
	case 0x01:
		return emitWithFlush(inputCursorMove{Dir: cursorHome})
	case 0x05:
		return emitWithFlush(inputCursorMove{Dir: cursorEnd})
	case 0x15:
		return emitWithFlush(inputErase{Kind: eraseToStart})
	case 0x0b:
		return emitWithFlush(inputErase{Kind: eraseToEnd})
	case 0x17:
		return emitWithFlush(inputErase{Kind: eraseWordBack})
	case 0x08, 0x7f:
		return emitWithFlush(inputErase{Kind: eraseCharBack})
	case '\n', '\r':
		return emitWithFlush(inputSubmit{})
	case '\t':
		p.pending = append(p.pending, b)
		return true
	}
	if (b >= 0x20 && b < 0x7f) || b >= 0x80 {
		p.pending = append(p.pending, b)
		return true
	}
	// Other control bytes (NUL, VT, FF, etc.) are dropped silently.
	return true
}

func (p *inputParser) handleEscByte(b byte, emit func(inputEvent) bool) bool {
	if len(p.escBuf) == 1 {
		// We have only the leading ESC; only "[" continues a CSI we recognize.
		if b != '[' {
			p.escBuf = p.escBuf[:0]
			return p.handleByte(b, emit)
		}
		p.escBuf = append(p.escBuf, b)
		return true
	}
	// In CSI: keep consuming until the final byte even if escBuf is full,
	// so an overlong unknown CSI doesn't leak its tail as printable input.
	if len(p.escBuf) < escBufMax {
		p.escBuf = append(p.escBuf, b)
	}
	if b >= 0x40 && b <= 0x7e {
		s := string(p.escBuf)
		p.escBuf = p.escBuf[:0]
		if e, ok := matchCSI(s); ok {
			return emit(e)
		}
	}
	return true
}

func matchCSI(s string) (inputEvent, bool) {
	switch s {
	case "\x1b[A":
		return inputHistoryUp{}, true
	case "\x1b[B":
		return inputHistoryDown{}, true
	case "\x1b[C":
		return inputCursorMove{Dir: cursorRight}, true
	case "\x1b[D":
		return inputCursorMove{Dir: cursorLeft}, true
	case "\x1b[200~":
		return inputPasteStart{}, true
	case "\x1b[201~":
		return inputPasteEnd{}, true
	}
	return nil, false
}

func runInputReader(r io.Reader, events chan<- inputEvent, stop <-chan struct{}) {
	defer close(events)
	emit := func(e inputEvent) bool {
		select {
		case events <- e:
			return true
		case <-stop:
			return false
		}
	}
	p := newInputParser()
	buf := make([]byte, 256)
	for {
		n, err := r.Read(buf)
		if n > 0 {
			if !p.parse(buf[:n], emit) {
				return
			}
			if !p.flushPrintable(emit) {
				return
			}
		}
		if err != nil {
			if !errors.Is(err, io.EOF) {
				select {
				case events <- inputError{Err: err}:
				case <-stop:
				}
			}
			return
		}
	}
}
