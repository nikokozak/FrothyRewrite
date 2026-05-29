//go:build darwin || linux

package main

import (
	"errors"
	"io"
)

// Stdin reader. Printable bytes, Enter, Ctrl-C, Ctrl-D only; the escape-
// sequence parser for arrow keys, cursor controls, and paste markers is
// not in this file yet (acceptance #2).

type inputEvent interface{ inputEventMarker() }

type inputPrintable struct{ Bytes []byte }
type inputSubmit struct{}
type inputInterrupt struct{}
type inputEOF struct{}
type inputError struct{ Err error }

func (inputPrintable) inputEventMarker() {}
func (inputSubmit) inputEventMarker()    {}
func (inputInterrupt) inputEventMarker() {}
func (inputEOF) inputEventMarker()       {}
func (inputError) inputEventMarker()     {}

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

	buf := make([]byte, 64)
	var pending []byte
	for {
		n, err := r.Read(buf)
		for i := 0; i < n; i++ {
			b := buf[i]
			switch {
			case b == 0x03:
				if len(pending) > 0 {
					if !emit(inputPrintable{Bytes: pending}) {
						return
					}
					pending = nil
				}
				if !emit(inputInterrupt{}) {
					return
				}
			case b == 0x04:
				if len(pending) > 0 {
					if !emit(inputPrintable{Bytes: pending}) {
						return
					}
					pending = nil
				}
				if !emit(inputEOF{}) {
					return
				}
			case b == '\n' || b == '\r':
				if len(pending) > 0 {
					if !emit(inputPrintable{Bytes: pending}) {
						return
					}
					pending = nil
				}
				if !emit(inputSubmit{}) {
					return
				}
			case b >= 0x20 && b < 0x7f, b >= 0x80:
				pending = append(pending, b)
			default:
				// Other control bytes (including ESC) wait for the parser.
			}
		}
		if len(pending) > 0 {
			if !emit(inputPrintable{Bytes: pending}) {
				return
			}
			pending = nil
		}
		if err != nil {
			if !errors.Is(err, io.EOF) {
				_ = emit(inputError{Err: err})
			}
			return
		}
	}
}
