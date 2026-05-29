//go:build darwin || linux

package main

import (
	"reflect"
	"testing"
)

func runParserAll(in []byte) []inputEvent {
	var got []inputEvent
	emit := func(e inputEvent) bool { got = append(got, e); return true }
	p := newInputParser()
	p.parse(in, emit)
	p.flushPrintable(emit)
	return got
}

// Every key in spec decision 2 (and the related paste / escape rules)
// maps to the documented event.
func TestInputParserKeySequences(t *testing.T) {
	cases := []struct {
		name string
		in   []byte
		want []inputEvent
	}{
		{"printable ascii", []byte("hi"), []inputEvent{inputPrintable{Bytes: []byte("hi")}}},
		{"printable high bit", []byte{0xc3, 0xa9}, []inputEvent{inputPrintable{Bytes: []byte{0xc3, 0xa9}}}},
		{"tab is printable", []byte{'\t'}, []inputEvent{inputPrintable{Bytes: []byte{'\t'}}}},
		{"enter LF", []byte{'\n'}, []inputEvent{inputSubmit{}}},
		{"enter CR", []byte{'\r'}, []inputEvent{inputSubmit{}}},
		{"backspace 0x7f", []byte{0x7f}, []inputEvent{inputErase{Kind: eraseCharBack}}},
		{"ctrl-H 0x08", []byte{0x08}, []inputEvent{inputErase{Kind: eraseCharBack}}},
		{"ctrl-A", []byte{0x01}, []inputEvent{inputCursorMove{Dir: cursorHome}}},
		{"ctrl-E", []byte{0x05}, []inputEvent{inputCursorMove{Dir: cursorEnd}}},
		{"ctrl-U", []byte{0x15}, []inputEvent{inputErase{Kind: eraseToStart}}},
		{"ctrl-K", []byte{0x0b}, []inputEvent{inputErase{Kind: eraseToEnd}}},
		{"ctrl-W", []byte{0x17}, []inputEvent{inputErase{Kind: eraseWordBack}}},
		{"ctrl-C", []byte{0x03}, []inputEvent{inputInterrupt{}}},
		{"ctrl-D", []byte{0x04}, []inputEvent{inputEOF{}}},
		{"left arrow", []byte("\x1b[D"), []inputEvent{inputCursorMove{Dir: cursorLeft}}},
		{"right arrow", []byte("\x1b[C"), []inputEvent{inputCursorMove{Dir: cursorRight}}},
		{"up arrow", []byte("\x1b[A"), []inputEvent{inputHistoryUp{}}},
		{"down arrow", []byte("\x1b[B"), []inputEvent{inputHistoryDown{}}},
		{"paste start", []byte("\x1b[200~"), []inputEvent{inputPasteStart{}}},
		{"paste end", []byte("\x1b[201~"), []inputEvent{inputPasteEnd{}}},
		{"control byte dropped", []byte{0x00, 'a'}, []inputEvent{inputPrintable{Bytes: []byte("a")}}},
		{
			"flush printable before control",
			[]byte("ab\x03"),
			[]inputEvent{inputPrintable{Bytes: []byte("ab")}, inputInterrupt{}},
		},
		{
			"flush printable before submit",
			[]byte("hello\r"),
			[]inputEvent{inputPrintable{Bytes: []byte("hello")}, inputSubmit{}},
		},
		{
			"flush printable around csi",
			[]byte("abc\x1b[Dxyz"),
			[]inputEvent{
				inputPrintable{Bytes: []byte("abc")},
				inputCursorMove{Dir: cursorLeft},
				inputPrintable{Bytes: []byte("xyz")},
			},
		},
		{
			"unknown csi dropped, trailing printable kept",
			[]byte("\x1b[Zhi"),
			[]inputEvent{inputPrintable{Bytes: []byte("hi")}},
		},
		{
			"bare esc then printable",
			[]byte{0x1b, 'x'},
			[]inputEvent{inputPrintable{Bytes: []byte{'x'}}},
		},
		{
			"bare esc then control",
			[]byte{0x1b, 0x03},
			[]inputEvent{inputInterrupt{}},
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := runParserAll(tc.in)
			if !reflect.DeepEqual(got, tc.want) {
				t.Fatalf("events = %#v, want %#v", got, tc.want)
			}
		})
	}
}

// Split escape across reads still emits one event for the full sequence.
func TestInputParserSplitEscapeAcrossReads(t *testing.T) {
	var got []inputEvent
	emit := func(e inputEvent) bool { got = append(got, e); return true }
	p := newInputParser()

	for _, chunk := range [][]byte{{0x1b}, []byte("["), []byte("A")} {
		p.parse(chunk, emit)
		p.flushPrintable(emit)
	}

	want := []inputEvent{inputHistoryUp{}}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("events = %#v, want %#v", got, want)
	}
}

// Paste markers split across reads still emit the paste-start/end pair.
func TestInputParserSplitPasteMarkers(t *testing.T) {
	var got []inputEvent
	emit := func(e inputEvent) bool { got = append(got, e); return true }
	p := newInputParser()

	for _, chunk := range [][]byte{[]byte("\x1b[20"), []byte("0~hi\x1b[20"), []byte("1~")} {
		p.parse(chunk, emit)
		p.flushPrintable(emit)
	}

	want := []inputEvent{
		inputPasteStart{},
		inputPrintable{Bytes: []byte("hi")},
		inputPasteEnd{},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("events = %#v, want %#v", got, want)
	}
}

// Overlong unknown CSI is consumed up to the final byte and dropped; bytes
// after the sequence return to normal parsing.
func TestInputParserOverlongCSIDropped(t *testing.T) {
	long := []byte("\x1b[")
	for i := 0; i < escBufMax+4; i++ {
		long = append(long, '0')
	}
	long = append(long, 'Z')
	long = append(long, 'h', 'i')

	got := runParserAll(long)
	want := []inputEvent{inputPrintable{Bytes: []byte("hi")}}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("events = %#v, want %#v", got, want)
	}
}
