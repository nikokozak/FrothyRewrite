package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

type sessionDevice interface {
	syncPrompt(timeout time.Duration) error
	sendLine(line string, timeout time.Duration, promptSeen func()) (string, error)
	interrupt(timeout time.Duration) (string, error)
}

type compilerMode string

const (
	compilerDevice compilerMode = "device"
)

type deviceStatus struct {
	profile     string
	profileHash string
	compiler    compilerMode
	names       string
	storage     string
	interrupt   string
	wordSize    uint16
	intMin      int64
	intMax      int64
	applyBytes  uint16
}

var errPromptTimeout = errors.New("timed out waiting for prompt")

const (
	deviceInterruptedStatus = "error: interrupted (10)"
	promptPrimary           = "frothy> "
	promptContinuation      = ".. "
)

var canonicalErrorStatusPattern = regexp.MustCompile(`^error: .+ \([0-9]+\)$`)
var canonicalNoticeStatusPattern = regexp.MustCompile(`^notice: .+ \([0-9]+\)$`)
var legacyErrorStatusPattern = regexp.MustCompile(`^err [0-9]+$`)

type serialDevice struct {
	file    *os.File
	readCh  chan byte
	errCh   chan error
	writeMu sync.Mutex
}

func configureSerial(file *os.File, baud int) error {
	args := []string{
		fmt.Sprintf("%d", baud),
		"raw",
		"-echo",
		"cs8",
		"-cstopb",
		"-parenb",
		"-ixon",
		"-ixoff",
	}
	cmd := exec.Command("stty", args...)
	var stderr bytes.Buffer
	cmd.Stdin = file
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		text := strings.TrimSpace(stderr.String())
		if text == "" {
			return err
		}
		return fmt.Errorf("%w: %s", err, text)
	}
	return nil
}

func openSerial(port string, baud int) (*serialDevice, error) {
	file, err := os.OpenFile(port, os.O_RDWR, 0)
	if err != nil {
		return nil, decorateSerialOpenError(port, err)
	}
	if err := setSerialExclusive(file); err != nil {
		_ = file.Close()
		return nil, decorateSerialOpenError(port, err)
	}
	if err := configureSerial(file, baud); err != nil {
		_ = file.Close()
		return nil, err
	}
	dev := &serialDevice{
		file:   file,
		readCh: make(chan byte, 256),
		errCh:  make(chan error, 1),
	}
	go dev.readLoop()
	return dev, nil
}

func (d *serialDevice) close() {
	_ = d.file.Close()
}

func (d *serialDevice) readLoop() {
	var buf [64]byte
	for {
		n, err := d.file.Read(buf[:])
		for i := 0; i < n; i++ {
			d.readCh <- buf[i]
		}
		if err != nil {
			d.errCh <- err
			return
		}
	}
}

func responseHasTerminalStatus(response string) bool {
	return isResponseStatus(responseStatus(response))
}

func promptComplete(text string, requireStatus bool) bool {
	if !strings.HasSuffix(text, "> ") {
		return false
	}
	if len(text) > len("> ") && text[len(text)-len("> ")-1] != '\n' {
		return false
	}
	if !requireStatus {
		return true
	}
	return responseHasTerminalStatus(strings.TrimSuffix(text, "> "))
}

func (d *serialDevice) readUntilPrompt(timeout time.Duration, requireStatus bool, promptSeen func()) (string, error) {
	var out strings.Builder
	deadline := time.NewTimer(timeout)
	defer deadline.Stop()

	for {
		select {
		case b := <-d.readCh:
			out.WriteByte(b)
			text := out.String()
			if promptComplete(text, requireStatus) {
				if promptSeen != nil {
					promptSeen()
				}
				return strings.TrimSuffix(text, "> "), nil
			}
		case err := <-d.errCh:
			return out.String(), err
		case <-deadline.C:
			return out.String(), errPromptTimeout
		}
	}
}

func (d *serialDevice) sendLine(line string, timeout time.Duration, promptSeen func()) (string, error) {
	if err := d.writeBytes([]byte(wireRequest(line) + "\n")); err != nil {
		return "", err
	}
	return d.readUntilPrompt(timeout, true, promptSeen)
}

func wireRequest(source string) string {
	normalized := strings.ReplaceAll(source, "\r\n", "\n")
	normalized = strings.ReplaceAll(normalized, "\r", "\n")
	if !sourceNeedsEnvelope(normalized) {
		return normalized
	}

	var request strings.Builder
	request.Grow(len(normalized) + len("source-form "))
	request.WriteString("source-form ")
	for i := 0; i < len(normalized); i++ {
		switch normalized[i] {
		case '\\':
			request.WriteString("\\\\")
		case '\n':
			request.WriteString("\\n")
		default:
			request.WriteByte(normalized[i])
		}
	}
	return request.String()
}

func sourceNeedsEnvelope(source string) bool {
	// The ESP console drops bytes outside printable ASCII. Keep any such source
	// behind the source-form prefix so its transformed echo cannot become wire
	// syntax (for example, a dropped tab turning "\tok" into "ok").
	for i := 0; i < len(source); i++ {
		if source[i] < 32 || source[i] > 126 {
			return true
		}
	}
	return strings.HasPrefix(source, "! ") ||
		strings.HasPrefix(source, "> ") ||
		strings.HasPrefix(source, "source-form ") ||
		canonicalNoticeStatusPattern.MatchString(source) ||
		isResponseStatus(source)
}

func (d *serialDevice) writeBytes(bytes []byte) error {
	d.writeMu.Lock()
	defer d.writeMu.Unlock()
	_, err := d.file.Write(bytes)
	return err
}

func (d *serialDevice) sendInterrupt() error {
	return d.writeBytes([]byte{0x03})
}

func (d *serialDevice) interrupt(timeout time.Duration) (string, error) {
	if err := d.sendInterrupt(); err != nil {
		return "", err
	}
	return d.readUntilPrompt(timeout, true, nil)
}

func (d *serialDevice) syncPrompt(timeout time.Duration) error {
	if _, err := d.readUntilPrompt(timeout, false, nil); err == nil {
		return nil
	}
	if err := d.writeBytes([]byte("\n")); err != nil {
		return err
	}
	_, err := d.readUntilPrompt(timeout, false, nil)
	return err
}

func responseStatus(response string) string {
	text := normalizeResponseText(response)
	initialPromptStatus := ""
	for index, line := range strings.Split(text, "\n") {
		if isResponseStatus(line) {
			return line
		}
		if index == 0 && strings.HasPrefix(line, "> ") {
			candidate := strings.TrimPrefix(line, "> ")
			if isResponseStatus(candidate) {
				initialPromptStatus = candidate
			}
		}
	}
	if initialPromptStatus != "" {
		return initialPromptStatus
	}
	return lastResponseStatusLine(text)
}

func isResponseStatus(line string) bool {
	return line == "ok" || canonicalErrorStatusPattern.MatchString(line) ||
		legacyErrorStatusPattern.MatchString(line)
}

func normalizeResponseText(response string) string {
	text := strings.ReplaceAll(response, "\r\n", "\n")
	return strings.ReplaceAll(text, "\r", "\n")
}

func lastResponseStatusLine(text string) string {
	lines := strings.Split(text, "\n")
	for i := len(lines) - 1; i >= 0; i-- {
		if strings.TrimSpace(lines[i]) != "" {
			return lines[i]
		}
	}
	return ""
}

func responseOK(response string) bool {
	return responseStatus(response) == "ok"
}

func responseNoticeStatus(response string) string {
	if !responseOK(response) {
		return ""
	}

	status := ""
	for _, line := range strings.Split(normalizeResponseText(response), "\n") {
		if canonicalNoticeStatusPattern.MatchString(line) {
			status = line
		}
	}
	return status
}

func responseNoticeText(response string) string {
	status := responseNoticeStatus(response)
	if status == "" {
		return ""
	}

	var lines []string
	for _, rawLine := range strings.Split(normalizeResponseText(response), "\n") {
		if rawLine == status {
			lines = []string{rawLine}
			continue
		}
		if len(lines) > 0 && strings.HasPrefix(rawLine, "detail:") {
			lines = append(lines, rawLine)
		}
	}
	return strings.Join(lines, "\n") + "\n"
}

func responseSettledAfterInterrupt(response string) bool {
	status := responseStatus(response)
	return status == "ok" || status == deviceInterruptedStatus
}

func promptRecoveredAfterInterrupt(partial string, recovery string) bool {
	return responseSettledAfterInterrupt(recovery) ||
		responseSettledAfterInterrupt(partial+recovery)
}

func validProfileHash(hash string) bool {
	if len(hash) != 8 {
		return false
	}
	for _, ch := range hash {
		if (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') {
			continue
		}
		return false
	}
	return true
}

func expectedIntRange(wordSize uint16) (int64, int64, bool) {
	// Keep these ranges in sync with the tagged int bands in src/tagged.h.
	switch wordSize {
	case 16:
		return -16384, 16383, true
	case 32:
		return -1073741824, 1073741823, true
	default:
		return 0, 0, false
	}
}

func validateIntRange(owner string, wordSize uint16, intMin int64, intMax int64) error {
	wantMin, wantMax, ok := expectedIntRange(wordSize)
	if !ok {
		return fmt.Errorf("%s unsupported word_size: %d", owner, wordSize)
	}
	if intMin != wantMin || intMax != wantMax {
		return fmt.Errorf("%s int range %d..%d does not match word_size=%d range %d..%d",
			owner, intMin, intMax, wordSize, wantMin, wantMax)
	}
	return nil
}

func parseDeviceStatus(response string) (deviceStatus, error) {
	if !responseOK(response) {
		return deviceStatus{}, fmt.Errorf("status failed: %s", responseStatus(response))
	}

	text := strings.ReplaceAll(response, "\r\n", "\n")
	text = strings.ReplaceAll(text, "\r", "\n")
	for _, rawLine := range strings.Split(text, "\n") {
		line := strings.TrimSpace(rawLine)
		if !strings.HasPrefix(line, "frothy status ") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 4 || fields[0] != "frothy" || fields[1] != "status" {
			return deviceStatus{}, fmt.Errorf("malformed status line: %s", line)
		}
		if fields[2] != "v1" {
			return deviceStatus{}, fmt.Errorf("unsupported status version: %s", fields[2])
		}

		values := make(map[string]string)
		for _, field := range fields[3:] {
			key, value, ok := strings.Cut(field, "=")
			if !ok || key == "" || value == "" {
				return deviceStatus{}, fmt.Errorf("malformed status field: %s", field)
			}
			values[key] = value
		}

		applyBytes, err := parseUint16Field(values, "apply_bytes")
		if err != nil {
			return deviceStatus{}, err
		}
		wordSize, err := parseUint16Field(values, "word_size")
		if err != nil {
			return deviceStatus{}, err
		}
		intMin, err := parseInt64Field(values, "int_min")
		if err != nil {
			return deviceStatus{}, err
		}
		intMax, err := parseInt64Field(values, "int_max")
		if err != nil {
			return deviceStatus{}, err
		}

		status := deviceStatus{
			profile:     values["profile"],
			profileHash: values["profile_hash"],
			compiler:    compilerMode(values["compiler"]),
			names:       values["names"],
			storage:     values["storage"],
			interrupt:   values["interrupt"],
			wordSize:    wordSize,
			intMin:      intMin,
			intMax:      intMax,
			applyBytes:  applyBytes,
		}
		if status.profile == "" || status.profile == "unknown" {
			return deviceStatus{}, errors.New("status missing profile")
		}
		if !validProfileHash(status.profileHash) {
			return deviceStatus{}, errors.New("status missing profile_hash")
		}
		if status.compiler != compilerDevice {
			return deviceStatus{}, fmt.Errorf("status unsupported compiler mode: %s", status.compiler)
		}
		if status.names == "" {
			return deviceStatus{}, errors.New("status missing names")
		}
		if status.storage == "" {
			return deviceStatus{}, errors.New("status missing storage")
		}
		if status.interrupt == "" {
			return deviceStatus{}, errors.New("status missing interrupt")
		}
		if status.interrupt != "cooperative" {
			return deviceStatus{}, fmt.Errorf("status unsupported interrupt mode: %s", status.interrupt)
		}
		if err := validateIntRange("status", status.wordSize, status.intMin, status.intMax); err != nil {
			return deviceStatus{}, err
		}
		return status, nil
	}

	return deviceStatus{}, errors.New("status response missing frothy status line")
}

func parseUint16Field(values map[string]string, name string) (uint16, error) {
	raw := values[name]
	if raw == "" {
		return 0, fmt.Errorf("missing %s", name)
	}
	value, err := strconv.ParseUint(raw, 10, 16)
	if err != nil {
		return 0, fmt.Errorf("invalid %s: %s", name, raw)
	}
	return uint16(value), nil
}

func parseInt64Field(values map[string]string, name string) (int64, error) {
	raw := values[name]
	if raw == "" {
		return 0, fmt.Errorf("missing %s", name)
	}
	value, err := strconv.ParseInt(raw, 10, 64)
	if err != nil {
		return 0, fmt.Errorf("invalid %s: %s", name, raw)
	}
	return value, nil
}

func retryableStatusResponse(response string, err error) bool {
	status := responseStatus(response)
	errText := ""
	if err != nil {
		errText = err.Error()
	}
	return status == "" ||
		status == "ok" && (errText == "status response missing frothy status line" ||
			strings.HasPrefix(errText, "malformed status "))
}

func readDeviceStatus(dev sessionDevice, timeout time.Duration) (deviceStatus, error) {
	var lastErr error
	deadline := time.Now().Add(timeout)

	for attempt := 0; attempt < 3; {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			if lastErr != nil {
				return deviceStatus{}, lastErr
			}
			return deviceStatus{}, errPromptTimeout
		}
		attemptTimeout := remaining
		if attemptTimeout > 300*time.Millisecond {
			attemptTimeout = 300 * time.Millisecond
		}
		response, err := dev.sendLine("status", attemptTimeout, nil)
		if err != nil {
			lastErr = err
			if errors.Is(err, errPromptTimeout) {
				continue
			}
			return deviceStatus{}, err
		}
		attempt++

		status, err := parseDeviceStatus(response)
		if err == nil {
			return status, nil
		}
		lastErr = err
		if !retryableStatusResponse(response, err) || attempt == 2 {
			return deviceStatus{}, err
		}
	}

	return deviceStatus{}, lastErr
}

type sourceFormState struct {
	lines          []string
	codeLines      []string
	parenDepth     int
	bracketDepth   int
	braceDepth     int
	inString       bool
	escaped        bool
	inBlockComment bool
}

func (s *sourceFormState) hasPending() bool {
	return len(s.lines) != 0
}

func (s *sourceFormState) reset() {
	s.lines = nil
	s.codeLines = nil
	s.parenDepth = 0
	s.bracketDepth = 0
	s.braceDepth = 0
	s.inString = false
	s.escaped = false
	s.inBlockComment = false
}

func (s *sourceFormState) appendLine(line string) (string, bool) {
	code := s.scan(line)
	codeTrimmed := strings.TrimSpace(code)
	if !s.hasPending() && codeTrimmed == "" {
		return "", false
	}

	s.lines = append(s.lines, line)
	s.codeLines = append(s.codeLines, codeTrimmed)

	source := s.source()
	codeSource := s.codeSource()
	if s.parenDepth == 0 && s.bracketDepth == 0 && s.braceDepth == 0 &&
		!s.inString && !s.inBlockComment && !sourceNeedsContinuation(codeSource) {
		s.reset()
		return source, true
	}
	return "", false
}

func (s *sourceFormState) source() string {
	return strings.TrimSpace(strings.Join(s.lines, "\n"))
}

func (s *sourceFormState) codeSource() string {
	parts := make([]string, 0, len(s.codeLines))
	for _, line := range s.codeLines {
		if line != "" {
			parts = append(parts, line)
		}
	}
	return strings.Join(parts, " ")
}

func (s *sourceFormState) scan(line string) string {
	var code strings.Builder

	for i := 0; i < len(line); i++ {
		ch := line[i]
		if s.inBlockComment {
			if ch == '*' && i+1 < len(line) && line[i+1] == '-' {
				s.inBlockComment = false
				i++
			}
			continue
		}
		if s.inString {
			code.WriteByte(ch)
			if s.escaped {
				s.escaped = false
				continue
			}
			if ch == '\\' {
				s.escaped = true
				continue
			}
			if ch == '"' {
				s.inString = false
			}
			continue
		}
		if ch == '-' && i+1 < len(line) && frothyCommentCanStart(line, i) {
			if line[i+1] == '-' {
				break
			}
			if line[i+1] == '*' {
				s.inBlockComment = true
				i++
				continue
			}
		}

		switch ch {
		case '"':
			s.inString = true
		case '(':
			s.parenDepth++
		case ')':
			if s.parenDepth > 0 {
				s.parenDepth--
			}
		case '[':
			s.bracketDepth++
		case ']':
			if s.bracketDepth > 0 {
				s.bracketDepth--
			}
		case '{':
			s.braceDepth++
		case '}':
			if s.braceDepth > 0 {
				s.braceDepth--
			}
		}
		code.WriteByte(ch)
	}
	return code.String()
}

func sourceNeedsContinuation(source string) bool {
	trimmed := strings.TrimSpace(source)
	if trimmed == "" {
		return false
	}
	if strings.HasSuffix(trimmed, ",") || strings.HasSuffix(trimmed, "->") {
		return true
	}

	fields := strings.Fields(trimmed)
	if len(fields) == 0 {
		return false
	}
	switch fields[len(fields)-1] {
	case "else", "fn", "forever", "if", "is", "repeat", "set", "to", "with":
		return true
	default:
		return false
	}
}

type sourceFormReader struct {
	scanner               *bufio.Scanner
	state                 sourceFormState
	queued                []string
	sourceBlockEndPending bool
	allowSourceBlocks     bool
}

type sourceFormRead struct {
	source         string
	sourceBlockEnd bool
}

func newSourceFormReader(input io.Reader) *sourceFormReader {
	return &sourceFormReader{scanner: bufio.NewScanner(input)}
}

func newSessionSourceFormReader(input io.Reader) *sourceFormReader {
	reader := newSourceFormReader(input)
	reader.allowSourceBlocks = true
	return reader
}

func (r *sourceFormReader) next(prompt io.Writer, interrupts *interruptTracker) (sourceFormRead, bool, error) {
	for {
		if len(r.queued) > 0 {
			source := r.queued[0]
			r.queued = r.queued[1:]
			return sourceFormRead{source: source}, true, nil
		}
		if r.sourceBlockEndPending {
			r.sourceBlockEndPending = false
			return sourceFormRead{sourceBlockEnd: true}, true, nil
		}
		if prompt != nil {
			if r.state.hasPending() {
				fmt.Fprint(prompt, promptContinuation)
			} else {
				fmt.Fprint(prompt, promptPrimary)
			}
		}
		if !r.scanner.Scan() {
			if err := r.scanner.Err(); err != nil {
				return sourceFormRead{}, false, err
			}
			if r.state.hasPending() {
				source := r.state.source()
				r.state.reset()
				return sourceFormRead{}, false, fmt.Errorf("incomplete top form: %s", source)
			}
			return sourceFormRead{}, false, nil
		}
		if interrupts != nil && interrupts.consumeIdle() {
			r.state.reset()
		}

		if r.allowSourceBlocks && !r.state.hasPending() {
			path, isSourceBlock := sourceBlockPath(r.scanner.Text())
			if isSourceBlock {
				forms, err := r.readSourceBlock(path)
				if err != nil {
					return sourceFormRead{}, false, err
				}
				r.queued = forms
				r.sourceBlockEndPending = true
				continue
			}
		}

		if source, complete := r.state.appendLine(r.scanner.Text()); complete {
			return sourceFormRead{source: source}, true, nil
		}
	}
}

func sourceBlockPath(line string) (string, bool) {
	trimmed := strings.TrimSpace(line)
	if trimmed == ".source" {
		return "", true
	}
	if !strings.HasPrefix(trimmed, ".source ") && !strings.HasPrefix(trimmed, ".source\t") {
		return "", false
	}
	return strings.TrimSpace(trimmed[len(".source"):]), true
}

func sourceBlockEnds(line string) bool {
	return strings.TrimSpace(line) == ".end-source"
}

func (r *sourceFormReader) readSourceBlock(path string) ([]string, error) {
	var text strings.Builder
	for r.scanner.Scan() {
		line := r.scanner.Text()
		if sourceBlockEnds(line) {
			return sourceFormsFromBlock(text.String(), path)
		}
		text.WriteString(line)
		text.WriteByte('\n')
	}
	if err := r.scanner.Err(); err != nil {
		return nil, err
	}
	return nil, errors.New(".source block missing .end-source")
}

func sourceFormsFromBlock(text string, path string) ([]string, error) {
	if path == "" {
		return sourceFormsFromText(text)
	}
	root := filepath.Clean(path)
	expanded, err := preprocessInclude(root, func(path string) (string, error) {
		if filepath.Clean(path) == root {
			return text, nil
		}
		return sourceLoader(path)
	})
	if err != nil {
		return nil, err
	}
	return sourceFormsFromText(expanded)
}

func collectSourceForms(input io.Reader) ([]string, error) {
	reader := newSourceFormReader(input)
	var forms []string
	for {
		read, ok, err := reader.next(nil, nil)
		if err != nil {
			return nil, err
		}
		if !ok {
			return forms, nil
		}
		if !read.sourceBlockEnd {
			forms = append(forms, read.source)
		}
	}
}

func isBootDefinition(line string) bool {
	inBlockComment := false
	fields := strings.Fields(stripFrothyComments(line, &inBlockComment))
	return len(fields) >= 2 && fields[0] == "boot" && fields[1] == "is"
}

func sourceFormsFromText(text string) ([]string, error) {
	forms, err := collectSourceForms(strings.NewReader(text))
	if err != nil {
		return nil, err
	}

	var nonBoot []string
	var boot []string
	for _, form := range forms {
		if isBootDefinition(form) {
			boot = append(boot, form)
		} else {
			nonBoot = append(nonBoot, form)
		}
	}
	return append(nonBoot, boot...), nil
}

func readFileLines(path string) ([]string, error) {
	bytes, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return sourceFormsFromBlock(string(bytes), path)
}

func readerFromLines(lines []string) io.Reader {
	var text strings.Builder

	for _, line := range lines {
		text.WriteString(line)
		text.WriteByte('\n')
	}
	return strings.NewReader(text.String())
}

type replayTranscriptRecord struct {
	session string
	seq     int64
	kind    recordKind
	state   recordState
	mirror  recordMirror
	source  string
	ok      bool
}

func readReplayLines(path string) ([]string, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	return replayLinesFromTranscript(file)
}

func replayLinesFromTranscript(input io.Reader) ([]string, error) {
	scanner := bufio.NewScanner(input)
	scanner.Buffer(make([]byte, 0, 4096), 1024*1024)

	var lines []string
	var pendingSource string
	var havePending bool
	var final replayTranscriptRecord
	var sawRecord bool
	var session string
	var wantSeq int64 = 1

	for lineNo := 1; scanner.Scan(); lineNo++ {
		record, err := parseReplayTranscriptRecord(scanner.Text(), lineNo)
		if err != nil {
			return nil, err
		}
		sawRecord = true
		final = record

		if record.seq != wantSeq {
			return nil, fmt.Errorf("transcript line %d has seq %d, want %d", lineNo, record.seq, wantSeq)
		}
		wantSeq++
		if session == "" {
			session = record.session
		} else if record.session != session {
			return nil, fmt.Errorf("transcript line %d changed session from %q to %q", lineNo, session, record.session)
		}

		if !knownReplayState(record.state) {
			return nil, fmt.Errorf("transcript line %d has unknown state %q", lineNo, record.state)
		}
		if !knownReplayMirror(record.mirror) {
			return nil, fmt.Errorf("transcript line %d has unknown mirror %q", lineNo, record.mirror)
		}
		if record.kind == recordSessionError {
			return nil, fmt.Errorf("transcript line %d is session_error", lineNo)
		}
		if record.state == recordStateError ||
			record.state == recordStateStale {
			return nil, fmt.Errorf("transcript line %d has %s state", lineNo, record.state)
		}
		if record.mirror == recordMirrorStale {
			return nil, fmt.Errorf("transcript line %d has stale mirror", lineNo)
		}

		// Unknown record kinds are observations for replay. They may affect an
		// editor view, but they are not source to send through the session path.
		switch record.kind {
		case recordSend:
			if havePending {
				return nil, errors.New("transcript has send before previous source settled")
			}
			pendingSource = record.source
			havePending = true
		case recordResponse:
			if havePending {
				if record.ok {
					lines = append(lines, pendingSource)
				}
				pendingSource = ""
				havePending = false
			}
		case recordInterrupt:
			pendingSource = ""
			havePending = false
		}
	}
	if err := scanner.Err(); err != nil {
		return nil, err
	}
	if !sawRecord {
		return nil, errors.New("transcript is empty")
	}
	if havePending {
		return nil, errors.New("transcript ended before sent source settled")
	}
	if final.kind != recordSessionEnd ||
		final.state != recordStateClosed ||
		final.mirror == recordMirrorStale {
		return nil, errors.New("transcript did not end cleanly")
	}
	return lines, nil
}

func parseReplayTranscriptRecord(line string, lineNo int) (replayTranscriptRecord, error) {
	if strings.TrimSpace(line) == "" {
		return replayTranscriptRecord{}, fmt.Errorf("transcript line %d is empty", lineNo)
	}

	var fields map[string]json.RawMessage
	if err := json.Unmarshal([]byte(line), &fields); err != nil {
		return replayTranscriptRecord{}, fmt.Errorf("transcript line %d is malformed JSON: %w", lineNo, err)
	}

	version, err := replayIntField(fields, "v")
	if err != nil {
		return replayTranscriptRecord{}, fmt.Errorf("transcript line %d: %w", lineNo, err)
	}
	if version != 1 {
		return replayTranscriptRecord{}, fmt.Errorf("transcript line %d has unsupported record version %d", lineNo, version)
	}
	session, err := replayStringField(fields, "session")
	if err != nil {
		return replayTranscriptRecord{}, fmt.Errorf("transcript line %d: %w", lineNo, err)
	}
	seq, err := replayIntField(fields, "seq")
	if err != nil {
		return replayTranscriptRecord{}, fmt.Errorf("transcript line %d: %w", lineNo, err)
	}
	if seq < 1 {
		return replayTranscriptRecord{}, fmt.Errorf("transcript line %d has invalid seq %d", lineNo, seq)
	}

	kind, err := replayStringField(fields, "kind")
	if err != nil {
		return replayTranscriptRecord{}, fmt.Errorf("transcript line %d: %w", lineNo, err)
	}
	state, err := replayStringField(fields, "state")
	if err != nil {
		return replayTranscriptRecord{}, fmt.Errorf("transcript line %d: %w", lineNo, err)
	}
	mirror, err := replayStringField(fields, "mirror")
	if err != nil {
		return replayTranscriptRecord{}, fmt.Errorf("transcript line %d: %w", lineNo, err)
	}

	record := replayTranscriptRecord{
		session: session,
		seq:     seq,
		kind:    recordKind(kind),
		state:   recordState(state),
		mirror:  recordMirror(mirror),
	}
	switch kind {
	case string(recordSend):
		source, err := replayStringField(fields, "source")
		if err != nil {
			return replayTranscriptRecord{}, fmt.Errorf("transcript line %d: %w", lineNo, err)
		}
		record.source = source
	case string(recordResponse):
		ok, err := replayBoolField(fields, "ok")
		if err != nil {
			return replayTranscriptRecord{}, fmt.Errorf("transcript line %d: %w", lineNo, err)
		}
		record.ok = ok
	}
	return record, nil
}

func replayStringField(fields map[string]json.RawMessage, name string) (string, error) {
	raw, ok := fields[name]
	if !ok {
		return "", fmt.Errorf("missing %s", name)
	}
	var value string
	if err := json.Unmarshal(raw, &value); err != nil {
		return "", fmt.Errorf("%s must be a string", name)
	}
	if value == "" {
		return "", fmt.Errorf("empty %s", name)
	}
	return value, nil
}

func replayIntField(fields map[string]json.RawMessage, name string) (int64, error) {
	raw, ok := fields[name]
	if !ok {
		return 0, fmt.Errorf("missing %s", name)
	}
	var value int64
	if err := json.Unmarshal(raw, &value); err != nil {
		return 0, fmt.Errorf("%s must be an integer", name)
	}
	return value, nil
}

func replayBoolField(fields map[string]json.RawMessage, name string) (bool, error) {
	raw, ok := fields[name]
	if !ok {
		return false, fmt.Errorf("missing %s", name)
	}
	var value bool
	if err := json.Unmarshal(raw, &value); err != nil {
		return false, fmt.Errorf("%s must be a boolean", name)
	}
	return value, nil
}

func knownReplayState(state recordState) bool {
	switch state {
	case recordStateSyncing, recordStateIdle, recordStateWaiting,
		recordStateInterrupting, recordStateStale, recordStateError,
		recordStateClosed:
		return true
	default:
		return false
	}
}

func knownReplayMirror(mirror recordMirror) bool {
	switch mirror {
	case recordMirrorNone, recordMirrorClean, recordMirrorPending,
		recordMirrorStale:
		return true
	default:
		return false
	}
}

func sameCleanPath(a string, b string) (bool, error) {
	leftInfo, leftErr := os.Stat(a)
	rightInfo, rightErr := os.Stat(b)
	if leftErr == nil && rightErr == nil {
		return os.SameFile(leftInfo, rightInfo), nil
	}

	left, err := resolvedCleanPath(a)
	if err != nil {
		return false, err
	}
	right, err := resolvedCleanPath(b)
	if err != nil {
		return false, err
	}
	return left == right, nil
}

func resolvedCleanPath(path string) (string, error) {
	left, err := filepath.Abs(path)
	if err != nil {
		return "", err
	}
	if resolved, err := filepath.EvalSymlinks(left); err == nil {
		return filepath.Clean(resolved), nil
	}

	// The transcript output path may not exist yet, so resolve its parent when
	// possible and compare the would-be file path under that directory.
	dir := filepath.Dir(left)
	base := filepath.Base(left)
	if resolvedDir, err := filepath.EvalSymlinks(dir); err == nil {
		return filepath.Clean(filepath.Join(resolvedDir, base)), nil
	}
	return filepath.Clean(left), nil
}

func validateSessionOptions(filePath string, records bool, transcript string, replay string) (int, error) {
	if transcript != "" && !records {
		return 2, errors.New("--transcript requires --records")
	}
	if replay != "" && filePath != "" {
		return 2, errors.New("--replay cannot be combined with --file")
	}
	// Replaying with records is allowed: it re-runs accepted source through the
	// normal live path while capturing a fresh observation stream.
	if replay != "" && transcript != "" {
		samePath, err := sameCleanPath(replay, transcript)
		if err != nil {
			return 1, fmt.Errorf("replay: %w", err)
		}
		if samePath {
			return 2, errors.New("--replay cannot write --transcript to the same path")
		}
	}
	return 0, nil
}

type recordWriter struct {
	encoder *json.Encoder
	session string
	seq     uint64
	mu      sync.Mutex
}

type recordKind string
type recordState string
type recordMirror string

const (
	recordSessionStart recordKind = "session_start"
	recordStatus       recordKind = "status"
	recordSend         recordKind = "send"
	recordCompileError recordKind = "compile_error"
	recordResponse     recordKind = "response"
	recordInterrupt    recordKind = "interrupt"
	recordSourceEnd    recordKind = "source_end"
	recordSessionError recordKind = "session_error"
	recordSessionEnd   recordKind = "session_end"
)

const (
	recordStateSyncing      recordState = "syncing"
	recordStateIdle         recordState = "idle"
	recordStateWaiting      recordState = "waiting"
	recordStateInterrupting recordState = "interrupting"
	recordStateStale        recordState = "stale"
	recordStateError        recordState = "error"
	recordStateClosed       recordState = "closed"
)

const (
	recordMirrorNone    recordMirror = "none"
	recordMirrorClean   recordMirror = "clean"
	recordMirrorPending recordMirror = "pending"
	recordMirrorStale   recordMirror = "stale"
)

const (
	recordErrorStatusFailed    = "status_failed"
	recordErrorSourceFailed    = "source_failed"
	recordErrorDeviceLost      = "device_lost"
	recordErrorInterruptFailed = "interrupt_failed"
	recordErrorNoPorts         = "no_ports"
	recordErrorMultiplePorts   = "multiple_ports"
)

func newRecordWriter(output io.Writer, session string) *recordWriter {
	encoder := json.NewEncoder(output)
	encoder.SetEscapeHTML(false)
	return &recordWriter{
		encoder: encoder,
		session: session,
	}
}

func openRecordOutput(stdout io.Writer, transcriptPath string) (io.Writer, func() error, error) {
	if transcriptPath == "" {
		return stdout, func() error { return nil }, nil
	}

	file, err := os.Create(transcriptPath)
	if err != nil {
		return nil, nil, err
	}
	return io.MultiWriter(stdout, file), file.Close, nil
}

func newSessionID() string {
	return fmt.Sprintf("s%d", time.Now().UnixNano())
}

func (w *recordWriter) write(kind recordKind, state recordState, mirror recordMirror, fields map[string]any) error {
	w.mu.Lock()
	defer w.mu.Unlock()

	w.seq += 1
	record := map[string]any{
		"v":       1,
		"session": w.session,
		"seq":     w.seq,
		"kind":    string(kind),
		"state":   string(state),
		"mirror":  string(mirror),
	}
	for key, value := range fields {
		switch key {
		case "v", "session", "seq", "kind", "state", "mirror":
			return fmt.Errorf("record field %q is reserved", key)
		}
		record[key] = value
	}
	return w.encoder.Encode(record)
}

func deviceStatusFields(status deviceStatus) map[string]any {
	return map[string]any{
		"profile":      status.profile,
		"profile_hash": status.profileHash,
		"compiler":     string(status.compiler),
		"names":        status.names,
		"storage":      status.storage,
		"interrupt":    status.interrupt,
		"word_size":    status.wordSize,
		"int_min":      status.intMin,
		"int_max":      status.intMax,
		"apply_bytes":  status.applyBytes,
	}
}

func (w *recordWriter) sessionStart() error {
	return w.write(recordSessionStart, recordStateSyncing, recordMirrorNone, nil)
}

func (w *recordWriter) status(status deviceStatus) error {
	return w.write(recordStatus, recordStateIdle, recordMirrorNone, map[string]any{
		"mode":   string(status.compiler),
		"device": deviceStatusFields(status),
	})
}

func (w *recordWriter) send(source string) error {
	return w.write(recordSend, recordStateWaiting, recordMirrorNone, map[string]any{
		"source": source,
		"line":   source,
		"action": "direct",
	})
}

func (w *recordWriter) response(response string) error {
	fields := map[string]any{
		"status": responseStatus(response),
		"ok":     responseOK(response),
		"text":   response,
	}
	if notice := responseNoticeStatus(response); notice != "" {
		fields["notice"] = notice
	}
	return w.write(recordResponse, recordStateIdle, recordMirrorNone, fields)
}

func (w *recordWriter) interrupt(state recordState, settled bool, text string, code string) error {
	fields := map[string]any{
		"settled": settled,
	}
	if text != "" {
		fields["text"] = text
		fields["status"] = responseStatus(text)
	}
	if code != "" {
		fields["code"] = code
	}
	return w.write(recordInterrupt, state, recordMirrorNone, fields)
}

func (w *recordWriter) sourceEnd() error {
	return w.write(recordSourceEnd, recordStateIdle, recordMirrorNone, nil)
}

func (w *recordWriter) sessionError(state recordState, code string, message string, candidates ...string) error {
	fields := map[string]any{
		"code":    code,
		"message": message,
	}
	if len(candidates) != 0 {
		fields["candidates"] = candidates
	}
	return w.write(recordSessionError, state, recordMirrorNone, fields)
}

func (w *recordWriter) sessionEnd() error {
	return w.write(recordSessionEnd, recordStateClosed, recordMirrorNone, nil)
}

type interruptTracker struct {
	mu            sync.Mutex
	active        bool
	requested     bool
	idleRequested bool
}

// The command loop owns the arm -> sendLine -> consume sequence. The signal
// goroutine only forwards raw Ctrl-C while a device command is active; idle
// interrupts clear pending host input without disturbing the device prompt.
func (t *interruptTracker) arm() {
	t.mu.Lock()
	t.active = true
	t.requested = false
	t.mu.Unlock()
}

func (t *interruptTracker) request() {
	_ = t.requestAndShouldForward()
}

func (t *interruptTracker) requestAndShouldForward() bool {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.active {
		t.requested = true
		return true
	} else {
		t.idleRequested = true
	}
	return false
}

func (t *interruptTracker) consume() bool {
	t.mu.Lock()
	requested := t.requested
	t.requested = false
	t.active = false
	t.mu.Unlock()
	return requested
}

func (t *interruptTracker) commandPromptSeen() bool {
	return t.consume()
}

func (t *interruptTracker) consumeIdle() bool {
	t.mu.Lock()
	requested := t.idleRequested
	t.idleRequested = false
	t.mu.Unlock()
	return requested
}

func runSerial(input io.Reader, output io.Writer, dev sessionDevice, timeout time.Duration) error {
	_, err := readDeviceStatus(dev, timeout)
	if err != nil {
		return err
	}
	return runSerialWithInterrupts(input, output, dev, timeout, nil)
}

func runSerialWithInterrupts(input io.Reader, output io.Writer, dev sessionDevice, timeout time.Duration, interrupts *interruptTracker) error {
	reader := newSessionSourceFormReader(input)
	for {
		read, ok, err := reader.next(output, interrupts)
		if err != nil {
			return err
		}
		if !ok {
			return nil
		}
		if read.sourceBlockEnd {
			continue
		}

		interrupted := false
		promptSeen := false
		var onPromptSeen func()
		if interrupts != nil {
			interrupts.arm()
			onPromptSeen = func() {
				interrupted = interrupts.commandPromptSeen()
				promptSeen = true
			}
		}
		response, err := dev.sendLine(read.source, timeout, onPromptSeen)
		if interrupts != nil && !promptSeen {
			interrupted = interrupts.consume()
		}
		if err != nil {
			if errors.Is(err, errPromptTimeout) {
				if err := handlePromptTimeout(output, dev, timeout, response); err != nil {
					return err
				}
				continue
			}
			return err
		}
		printDeviceResponse(output, response)
		if interrupted {
			if err := handleSignalInterrupt(response); err != nil {
				return err
			}
		}
	}
}

func handleSignalInterrupt(response string) error {
	if !responseSettledAfterInterrupt(response) {
		return fmt.Errorf("interrupt recovery returned %q; session state unknown", responseStatus(response))
	}
	return nil
}

func runSerialRecords(input io.Reader, records *recordWriter, dev sessionDevice, timeout time.Duration, interrupts *interruptTracker) error {
	if interrupts == nil {
		return errors.New("record session requires interrupt tracker")
	}

	reader := newSessionSourceFormReader(input)
	for {
		read, ok, err := reader.next(nil, interrupts)
		if err != nil {
			_ = records.sessionError(recordStateError, recordErrorSourceFailed, err.Error())
			return err
		}
		if !ok {
			return records.sessionEnd()
		}
		if read.sourceBlockEnd {
			if err := records.sourceEnd(); err != nil {
				return err
			}
			continue
		}

		if err := records.send(read.source); err != nil {
			return err
		}

		interrupted := false
		promptSeen := false
		interrupts.arm()
		response, err := dev.sendLine(read.source, timeout, func() {
			interrupted = interrupts.commandPromptSeen()
			promptSeen = true
		})
		if !promptSeen {
			interrupted = interrupts.consume()
		}
		if err != nil {
			if errors.Is(err, errPromptTimeout) {
				if err := handleRecordPromptTimeout(records, dev, timeout, response); err != nil {
					return err
				}
				continue
			}
			return handleRecordDeviceError(records, err)
		}
		if interrupted {
			if err := handleRecordSignalInterrupt(records, response); err != nil {
				return err
			}
			continue
		}
		if err := records.response(response); err != nil {
			return err
		}
	}
}

func handleRecordDeviceError(records *recordWriter, err error) error {
	_ = records.sessionError(recordStateError, recordErrorDeviceLost, err.Error())
	return err
}

func handleRecordPromptTimeout(records *recordWriter, dev sessionDevice, timeout time.Duration, partial string) error {
	recovery, recoverErr := dev.interrupt(timeout)
	text := partial + recovery
	settled := recoverErr == nil && promptRecoveredAfterInterrupt(partial, recovery)

	if recoverErr != nil {
		err := fmt.Errorf("interrupt recovery failed: %w", recoverErr)
		_ = records.interrupt(recordStateError, false, text, recordErrorInterruptFailed)
		_ = records.sessionError(recordStateError, recordErrorInterruptFailed, err.Error())
		return err
	}
	if !settled {
		err := fmt.Errorf("interrupt recovery returned %q; session state unknown", responseStatus(text))
		_ = records.interrupt(recordStateError, false, text, recordErrorInterruptFailed)
		_ = records.sessionError(recordStateError, recordErrorInterruptFailed, err.Error())
		return err
	}
	return records.interrupt(recordStateIdle, true, text, "")
}

func handleRecordSignalInterrupt(records *recordWriter, response string) error {
	if !responseSettledAfterInterrupt(response) {
		err := fmt.Errorf("interrupt recovery returned %q; session state unknown", responseStatus(response))
		_ = records.interrupt(recordStateError, false, response, recordErrorInterruptFailed)
		_ = records.sessionError(recordStateError, recordErrorInterruptFailed, err.Error())
		return err
	}
	return records.interrupt(recordStateIdle, true, response, "")
}

func printDeviceResponse(output io.Writer, response string) {
	fmt.Fprint(output, response)
	if response != "" && !strings.HasSuffix(response, "\n") {
		fmt.Fprintln(output)
	}
}

func handlePromptTimeout(output io.Writer, dev sessionDevice, timeout time.Duration, partial string) error {
	recovery, recoverErr := dev.interrupt(timeout)
	recoveredResponse := partial + recovery
	printDeviceResponse(output, recoveredResponse)

	if recoverErr != nil {
		return fmt.Errorf("interrupt recovery failed: %w", recoverErr)
	}
	if !promptRecoveredAfterInterrupt(partial, recovery) {
		return fmt.Errorf("interrupt recovery returned %q; session state unknown", responseStatus(recoveredResponse))
	}
	return nil
}

func forwardInterruptSignals(dev interface{ sendInterrupt() error }, stderr io.Writer, tracker *interruptTracker) func() {
	signals := make(chan os.Signal, 1)
	done := make(chan struct{})

	signal.Notify(signals, os.Interrupt)
	go func() {
		for {
			select {
			case <-done:
				return
			case <-signals:
				forward := true
				if tracker != nil {
					forward = tracker.requestAndShouldForward()
				}
				if !forward {
					continue
				}
				if err := dev.sendInterrupt(); err != nil && stderr != nil {
					fmt.Fprintf(stderr, "interrupt: %v\n", err)
				}
			}
		}
	}()

	return func() {
		signal.Stop(signals)
		close(done)
	}
}

// portLister returns candidate device paths. Injected so tests can drive
// discovery without touching the host filesystem.
type portLister func() ([]string, error)

func defaultPortLister() ([]string, error) {
	entries, err := os.ReadDir("/dev")
	if err != nil {
		return nil, err
	}
	paths := make([]string, 0, len(entries))
	for _, e := range entries {
		paths = append(paths, filepath.Join("/dev", e.Name()))
	}
	return paths, nil
}

// isLikelySerialPort matches the USB-serial naming used on macOS
// (cu.usbmodem*, cu.usbserial*) and Linux (ttyUSB*, ttyACM*).
func isLikelySerialPort(path string) bool {
	base := filepath.Base(path)
	switch {
	case strings.HasPrefix(base, "cu.usbmodem"),
		strings.HasPrefix(base, "cu.usbserial"),
		strings.HasPrefix(base, "ttyUSB"),
		strings.HasPrefix(base, "ttyACM"):
		return true
	}
	return false
}

type portSelectionError struct {
	code       string
	candidates []string
}

func (e *portSelectionError) Error() string {
	if e.code == recordErrorMultiplePorts {
		return fmt.Sprintf("multiple serial ports found: %s; pass --port to choose",
			strings.Join(e.candidates, ", "))
	}
	return "no serial port found; pass --port"
}

// pickPort returns override if it is set. Otherwise it asks list for
// candidates and returns the single USB-serial match. Zero or many matches
// each return an error that tells the user what to do next.
func pickPort(override string, list portLister) (string, error) {
	if override != "" {
		return override, nil
	}
	paths, err := list()
	if err != nil {
		return "", err
	}
	var matches []string
	for _, p := range paths {
		if isLikelySerialPort(p) {
			matches = append(matches, p)
		}
	}
	switch len(matches) {
	case 0:
		return "", &portSelectionError{code: recordErrorNoPorts}
	case 1:
		return matches[0], nil
	default:
		return "", &portSelectionError{
			code:       recordErrorMultiplePorts,
			candidates: append([]string(nil), matches...),
		}
	}
}

func pickSessionPort(override string, list portLister, records *recordWriter) (string, error) {
	chosen, err := pickPort(override, list)
	if err == nil || records == nil {
		return chosen, err
	}
	var selection *portSelectionError
	if errors.As(err, &selection) {
		_ = records.sessionError(recordStateError, selection.code,
			selection.Error(), selection.candidates...)
	}
	return "", err
}

type verb struct {
	name     string
	group    string
	summary  string
	longDesc string
	examples string
	run      func() int
}

func availableVerbs() []verb {
	return []verb{
		{name: "menu", group: "Start", summary: "choose common Frothy workflows from a numbered terminal menu", run: runMenuMain,
			longDesc: "Menu is a small numbered guide for common Frothy workflows. It does " +
				"not own device, build, flash, or serial behavior; it prints the equivalent " +
				"command and runs the existing verb. Use it when you want help choosing the " +
				"next step, then graduate to the verbs it shows.",
			examples: "  frothy menu\n" +
				"      open the numbered setup, connect, and recovery menu"},
		{name: "send", group: "Work", summary: "send a source file through the board's compiler", run: runSendMain,
			longDesc: "Send sends a Frothy source file to a connected board over serial. The " +
				"board compiles each definition and runs bare expressions line by line. " +
				"Use it for one-shot delivery of a file you have already written, as opposed " +
				"to session or connect, which keep an interactive prompt open. It talks to the " +
				"device through the serial port named by --port, at the baud rate given by " +
				"--baud.",
			examples: "  frothy send blink.fr --port /dev/cu.usbserial-0001\n" +
				"      send blink.fr through the compiler on the board on that port"},
		{name: "flash", group: "Project", summary: "flash Frothy firmware to a board over serial", run: runFlashMain,
			longDesc: "Flash writes Frothy firmware to a connected " +
				"device over serial, replacing whatever was on the chip. Use it once per board " +
				"to install Frothy itself, before send or connect can talk to a running device. " +
				"A packaged install uses the release firmware included by Homebrew; from a Frothy " +
				"source checkout it builds that checkout first. " +
				"The serial port is discovered automatically when one device is attached, or " +
				"named explicitly with --port when several are.",
			examples: "  frothy flash esp32_devkit_v1 --port /dev/cu.usbserial-0001\n" +
				"      flash Frothy to the board on that port"},
		{name: "wipe", group: "Recover", summary: "erase Frothy persistence on a wedged board", run: runWipeMain,
			longDesc: "Wipe erases the persisted device state on a board wedged by a bad save, " +
				"leaving the firmware and other partitions in place. It erases the dedicated " +
				"Frothy persistence partition and requires --force so it cannot run by accident. " +
				"Use it when a board refuses to boot the REPL because of corrupt persisted state; " +
				"for a clean reset of user definitions on a running device, prefer wipe-user.",
			examples: "  frothy wipe esp32_devkit_v1 --force --port /dev/cu.usbserial-0001\n" +
				"      erase Frothy persistence on the board on that port"},
		{name: "wipe-user", group: "Recover", summary: "clear user-tier definitions on a running device; library tier survives", run: runWipeUserMain,
			longDesc: "Wipe-user clears the user-tier definitions on a running device, leaving " +
				"the library tier and the firmware in place. Use it when you want to start with " +
				"a clean slate of your own definitions without touching library code the " +
				"project installed. It opens the serial port and asks the device to drop its " +
				"user overlay.",
			examples: "  frothy wipe-user --port /dev/cu.usbserial-0001\n" +
				"      clear user-tier definitions on the board on that port"},
		{name: "doctor", group: "Start", summary: "check the installed CLI, device, and available firmware tools", run: runDoctorMain,
			longDesc: "Doctor checks serial discovery and the connected device. When a " +
				"Frothy source checkout is available, it also checks the Make and ESP-IDF tools " +
				"used for firmware development. A package installation does not need those " +
				"source-build checks for flash, connect, send, or editor use. It does not modify anything.",
			examples: "  frothy doctor\n" +
				"      run every check and print the results"},
		{name: "connect", group: "Work", summary: "open the human REPL over serial", run: runConnectMain,
			longDesc: "Connect is Frothy's human REPL over serial. Use it to type at a running " +
				"board and read replies, with line history and a status probe that retries while " +
				"the board is waking. For one-shot delivery of a file, use send. Editors use " +
				"session instead.",
			examples: "  frothy connect --port /dev/cu.usbserial-0001\n" +
				"      open the human REPL on the board on that port"},
		{name: "stop", group: "Recover", summary: "stop Frothy sessions that are holding serial ports", run: runStopMain,
			longDesc: "Stop finds Frothy processes that are holding serial ports and asks them " +
				"to exit so another command can use the board. It uses the system's process " +
				"and open-file tables, not a Frothy registry, and it never stops non-Frothy " +
				"processes.",
			examples: "  frothy stop\n" +
				"      stop Frothy serial sessions so their ports can be reopened"},
		{name: "init", group: "Project", summary: "scaffold a new Frothy project in the current directory", run: runInitMain,
			longDesc: "Init scaffolds a new Frothy project in the current directory: a " +
				"frothy.toml with the project name and board, a main.fr with a starter " +
				"definition, and a .gitignore that excludes the local cache. It refuses to " +
				"overwrite existing files, so it is safe to run by accident.",
			examples: "  frothy init\n" +
				"      create frothy.toml, main.fr, and .gitignore in the current directory"},
		{name: "build", group: "Project", summary: "resolve libraries and build the project's firmware", run: runBuildMain,
			longDesc: "Build resolves the project's libraries from frothy.toml, generates the " +
				"board-specific sources, and runs make to produce the firmware image for the board " +
				"named in the manifest. Missing git dependencies are fetched into the local " +
				"cache first. Use it when you have changed library code or board settings and " +
				"want to produce a flashable artifact without flashing it.",
			examples: "  frothy build\n" +
				"      build the project in the current directory"},
		{name: "fetch", group: "Project", summary: "fetch git deps into the local cache", run: runFetchMain,
			longDesc: "Fetch resolves git dependencies declared in frothy.toml into the local " +
				"cache so build can find them offline. Each git dep must name a pinned rev; " +
				"branch tracking is deliberately not part of this command.",
			examples: "  frothy fetch\n" +
				"      clone missing git deps and check out their pinned revs"},
		{name: "install", group: "Project", summary: "send the project's library source to a device under install-library mode", run: runInstallMain,
			longDesc: "Install sends the project's library source to a connected device under " +
				"install-library mode, persisting the library tier on the board. Use it after " +
				"build to deliver the library code your project depends on to the device. The " +
				"user tier is untouched.",
			examples: "  frothy install --port /dev/cu.usbserial-0001\n" +
				"      send the project's library source to the board on that port"},
		{name: "bootstrap", group: "Start", summary: "fetch and install the ESP-IDF SDK under ~/.froth/sdk/", run: runBootstrapMain,
			longDesc: "Bootstrap fetches and installs the ESP-IDF SDK that source-checkout flash, build, and " +
				"firmware bundle generation depend on, placing it under ~/.froth/sdk/esp-idf/ (or under $FROTH_HOME " +
				"if set). It uses no sudo, never writes outside that tree, and is idempotent: a " +
				"successful install drops a marker file, and a re-run exits 0 with \"already " +
				"installed.\" Pass --force to reinstall from scratch.",
			examples: "  frothy bootstrap\n" +
				"      install ESP-IDF v5.5 under ~/.froth/sdk/ (skip if already installed)"},
		{name: "session", group: "Editor plumbing", summary: "run the structured editor session", run: runSessionMain,
			longDesc: "Session is the structured serial path for editors. It accepts " +
				".source/.end-source blocks, sends source through the board's compiler, replays " +
				"recorded transcripts, or emits NDJSON records. Human terminal use should " +
				"normally start with connect.",
			examples: "  frothy session --records --port /dev/cu.usbserial-0001\n" +
				"      emit the structured session records consumed by editor clients\n\n" +
				"  printf '.source main.fr\\ninclude \"helper.fr\"\\nmain:\\n.end-source\\n' | frothy session --port /dev/cu.usbserial-0001\n" +
				"      send unsaved source text as one editor-owned block"},
	}
}

func runSendMain() int {
	return runSendCommand(os.Args[1:], os.Stdout, os.Stderr)
}

func runSendCommand(args []string, stdout io.Writer, stderr io.Writer) int {
	fs := flag.NewFlagSet("frothy send", flag.ContinueOnError)
	fs.SetOutput(stderr)
	var (
		port    = fs.String("port", "", "serial port, for example /dev/cu.usbmodem101")
		baud    = fs.Int("baud", 115200, "serial baud rate")
		timeout = fs.Duration("timeout", 3*time.Second, "serial prompt timeout")
		settle  = fs.Duration("settle", 2*time.Second, "delay after opening serial")
	)
	if helpRequested(args) {
		printVerbHelp(stdout, helpFor("send"), fs)
		return 0
	}
	var positional []string
	remaining := args
	for {
		if err := fs.Parse(remaining); err != nil {
			if errors.Is(err, flag.ErrHelp) {
				return 0
			}
			return 2
		}
		rest := fs.Args()
		if len(rest) == 0 {
			break
		}
		positional = append(positional, rest[0])
		remaining = rest[1:]
	}
	if len(positional) != 1 {
		fmt.Fprintln(stderr, "send: expected exactly one source file")
		return 2
	}
	path := positional[0]

	if *port == "" {
		fmt.Fprintln(stderr, "send: --port is required")
		return 2
	}

	info, err := os.Stat(path)
	if err != nil {
		fmt.Fprintf(stderr, "send: %v\n", err)
		return 1
	}
	if info.IsDir() {
		fmt.Fprintf(stderr, "send: %s is not a file\n", path)
		return 1
	}

	// Reuse session's --file path so file ordering and serial handling stay in
	// one place.
	sessionArgs := []string{os.Args[0], "--file", path, "--port", *port,
		"--baud", strconv.Itoa(*baud),
		"--timeout", timeout.String(), "--settle", settle.String()}
	oldArgs := os.Args
	os.Args = sessionArgs
	defer func() { os.Args = oldArgs }()
	oldFlag := flag.CommandLine
	flag.CommandLine = flag.NewFlagSet(sessionArgs[0], flag.ExitOnError)
	defer func() { flag.CommandLine = oldFlag }()
	return runSessionMain()
}

// commandRunner runs an external command. Injected so flash tests can capture
// the make argv instead of executing make.
type commandRunner func(name string, args []string) error

type commandOutputError struct {
	err    error
	output string
}

func (e commandOutputError) Error() string { return e.err.Error() }

func (e commandOutputError) Unwrap() error { return e.err }

func commandErrorText(err error) string {
	text := err.Error()
	var outputErr commandOutputError
	if errors.As(err, &outputErr) && outputErr.output != "" {
		text += "\n" + outputErr.output
	}
	return text
}

func defaultCommandRunner(name string, args []string) error {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	cmd := exec.Command(name, args...)
	cmd.Stdout = io.MultiWriter(os.Stdout, &stdout)
	cmd.Stderr = io.MultiWriter(os.Stderr, &stderr)
	cmd.Stdin = os.Stdin
	if err := cmd.Run(); err != nil {
		return commandOutputError{err: err, output: stdout.String() + stderr.String()}
	}
	return nil
}

func runFlashMain() int {
	args := os.Args[1:]
	root := ""
	firmwareRoot := ""
	if !helpRequested(args) {
		root, _ = resolveFrothySourceRoot(".")
		executable, _ := os.Executable()
		firmwareRoot = packagedFirmwareRoot(executable)
	}
	return runFlashCommand(args, root, firmwareRoot, os.Stdout, os.Stderr,
		defaultPortLister, defaultCommandRunner)
}

func runFlashCommand(args []string, sourceRoot, firmwareRoot string, stdout io.Writer,
	stderr io.Writer, list portLister, run commandRunner) int {
	fs := flag.NewFlagSet("frothy flash", flag.ContinueOnError)
	fs.SetOutput(stderr)
	port := fs.String("port", "", "serial port, for example /dev/cu.usbmodem101")

	if helpRequested(args) {
		printVerbHelp(stdout, helpFor("flash"), fs)
		return 0
	}

	var positional []string
	remaining := args
	for {
		if err := fs.Parse(remaining); err != nil {
			if errors.Is(err, flag.ErrHelp) {
				return 0
			}
			return 2
		}
		rest := fs.Args()
		if len(rest) == 0 {
			break
		}
		positional = append(positional, rest[0])
		remaining = rest[1:]
	}
	if len(positional) != 1 {
		fmt.Fprintln(stderr, "flash: expected exactly one board name")
		return 2
	}
	board := positional[0]

	var packaged firmwareBundleRow
	if sourceRoot != "" {
		if !flashableBoard(filepath.Join(sourceRoot, "boards"), board) {
			fmt.Fprintf(stderr, "flash: unknown board %q\n", board)
			return 2
		}
	} else {
		if firmwareRoot == "" {
			fmt.Fprintln(stderr, "flash: no packaged firmware is installed; reinstall Frothy with Homebrew or run from a Frothy source checkout")
			return 2
		}
		var err error
		packaged, err = loadPackagedFirmware(firmwareRoot, board)
		if err != nil {
			fmt.Fprintf(stderr, "flash: %v\n", err)
			return 2
		}
	}

	chosen, err := pickPort(*port, list)
	if err != nil {
		fmt.Fprintf(stderr, "flash: %v\n", err)
		return 2
	}

	command := "make"
	commandArgs := []string{"-C", sourceRoot, "flash", "BOARD=" + board, "BOARD_PORT=" + chosen}
	if sourceRoot == "" {
		command = "esptool"
		commandArgs = []string{"--chip", packaged.Chip, "--port", chosen, "--baud", "460800",
			"--before", "default-reset", "--after", "hard-reset", "write-flash",
			"--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep"}
		for _, segment := range packaged.Segments {
			commandArgs = append(commandArgs, fmt.Sprintf("0x%x", segment.Address),
				filepath.Join(firmwareRoot, segment.File))
		}
	}

	if err := run(command, commandArgs); err != nil {
		fmt.Fprintf(stderr, "flash: %v\n", err)
		if strings.Contains(strings.ToLower(commandErrorText(err)), "busy") {
			fmt.Fprintln(stderr, "flash: port is busy; try: frothy stop")
		}
		return 1
	}
	return 0
}

func runWipeMain() int {
	args := os.Args[1:]
	root := ""
	if !helpRequested(args) {
		var err error
		root, err = resolveFrothySourceRoot(".")
		if err != nil {
			fmt.Fprintf(os.Stderr, "wipe: %v\n", err)
			return 2
		}
	}
	return runWipeCommand(args, root, os.Stdout, os.Stderr, defaultPortLister, defaultCommandRunner)
}

func wipeRecoveryHint(port string) string {
	return fmt.Sprintf("choose the board explicitly: frothy wipe --force BOARD --port %s", port)
}

func runWipeCommand(args []string, sourceRoot string, stdout io.Writer,
	stderr io.Writer, list portLister, run commandRunner) int {
	fs := flag.NewFlagSet("frothy wipe", flag.ContinueOnError)
	fs.SetOutput(stderr)
	port := fs.String("port", "", "serial port, for example /dev/cu.usbserial-0001")
	force := fs.Bool("force", false, "required: erase persisted state on the board")

	if helpRequested(args) {
		printVerbHelp(stdout, helpFor("wipe"), fs)
		return 0
	}

	var positional []string
	remaining := args
	for {
		if err := fs.Parse(remaining); err != nil {
			if errors.Is(err, flag.ErrHelp) {
				return 0
			}
			return 2
		}
		rest := fs.Args()
		if len(rest) == 0 {
			break
		}
		positional = append(positional, rest[0])
		remaining = rest[1:]
	}
	if len(positional) != 1 {
		fmt.Fprintln(stderr, "wipe: expected exactly one board name")
		return 2
	}
	board := positional[0]

	if sourceRoot == "" {
		fmt.Fprintf(stderr, "wipe: cannot find Frothy source root; set %s\n", frothySourceRootEnv)
		return 2
	}
	if !flashableBoard(filepath.Join(sourceRoot, "boards"), board) {
		fmt.Fprintf(stderr, "wipe: unsupported board %q\n", board)
		return 2
	}

	if !*force {
		shown := *port
		if shown == "" {
			shown = "<port>"
		}
		fmt.Fprintln(stderr, "wipe: refusing to erase persisted device state without --force")
		fmt.Fprintf(stderr, "      frothy wipe --force %s --port %s\n", board, shown)
		return 2
	}

	chosen, err := pickPort(*port, list)
	if err != nil {
		fmt.Fprintf(stderr, "wipe: %v\n", err)
		return 2
	}

	if err := run("make", []string{"-C", sourceRoot, "wipe-persist", "BOARD=" + board, "BOARD_PORT=" + chosen}); err != nil {
		fmt.Fprintf(stderr, "wipe: %v\n", err)
		return 1
	}
	return 0
}

type doctorCheck struct {
	name string
	run  func() (bool, string)
}

func formatDoctorDeviceFailure(port string, err error) string {
	return fmt.Sprintf("no status from %s: %v", port, err)
}

func formatDoctorUnresponsiveDevice(port string, err error) string {
	return formatDoctorDeviceFailure(port, err) + " (expected before first flash)"
}

func defaultDoctorChecks() []doctorCheck {
	_, sourceErr := resolveFrothySourceRoot(".")
	return doctorChecks(sourceErr == nil)
}

func doctorChecks(includeFirmware bool) []doctorCheck {
	var checks []doctorCheck
	if includeFirmware {
		checks = append(checks, doctorCheck{
			name: "make",
			run: func() (bool, string) {
				path, err := exec.LookPath("make")
				if err != nil {
					return false, "not on PATH; install build tools"
				}
				return true, path
			},
		})
	}
	checks = append(checks,
		doctorCheck{
			name: "serial",
			run: func() (bool, string) {
				chosen, err := pickPort("", defaultPortLister)
				if err != nil {
					return false, err.Error()
				}
				return true, chosen
			},
		},
		doctorCheck{
			name: "device",
			run: func() (bool, string) {
				port, err := pickPort("", defaultPortLister)
				if err != nil {
					return false, err.Error()
				}
				dev, err := openSerial(port, 115200)
				if err != nil {
					return false, formatDoctorDeviceFailure(port, err)
				}
				defer dev.close()
				time.Sleep(2 * time.Second)
				if _, err := readDeviceStatus(dev, 3*time.Second); err != nil {
					return false, formatDoctorUnresponsiveDevice(port, err)
				}
				return true, port
			},
		},
	)
	if includeFirmware {
		checks = append(checks, doctorCheck{
			name: "esp-idf-installed",
			run: func() (bool, string) {
				home := os.Getenv("FROTH_HOME")
				if home == "" {
					home = filepath.Join(os.Getenv("HOME"), ".froth")
				}
				marker := filepath.Join(home, "sdk", "esp-idf", ".froth-install-complete")
				if _, err := os.Stat(marker); err != nil {
					return false, "not installed; run frothy bootstrap"
				}
				return true, filepath.Join(home, "sdk", "esp-idf")
			},
		})
	}
	return checks
}

func runDoctorMain() int {
	return runDoctorCommand(os.Args[1:], os.Stdout, os.Stderr, defaultDoctorChecks())
}

func runDoctorCommand(args []string, stdout io.Writer, stderr io.Writer, checks []doctorCheck) int {
	fs := flag.NewFlagSet("frothy doctor", flag.ContinueOnError)
	fs.SetOutput(stderr)
	if helpRequested(args) {
		printVerbHelp(stdout, helpFor("doctor"), fs)
		return 0
	}
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if fs.NArg() != 0 {
		fmt.Fprintln(stderr, "doctor: takes no positional arguments")
		return 2
	}
	failed := 0
	for _, c := range checks {
		ok, detail := c.run()
		label := "ok"
		if !ok {
			label = "fail"
			failed += 1
		}
		if detail == "" {
			fmt.Fprintf(stdout, "%-4s  %s\n", label, c.name)
		} else {
			fmt.Fprintf(stdout, "%-4s  %s: %s\n", label, c.name, detail)
		}
	}
	if failed > 0 {
		return 1
	}
	return 0
}

// helpRequested reports whether the args ask for help. The flag package would
// treat -h/--help as a help request, but it prints to stderr and gives no
// description; we intercept first so help is curated and goes to stdout.
func helpRequested(args []string) bool {
	for _, a := range args {
		if a == "--" {
			return false
		}
		if a == "-h" || a == "-help" || a == "--help" {
			return true
		}
	}
	return false
}

// helpFor returns the verb metadata so a run function can print its own help
// without restating its summary. availableVerbs is the single source of truth.
func helpFor(name string) verb {
	for _, v := range availableVerbs() {
		if v.name == name {
			return v
		}
	}
	return verb{name: name}
}

// printVerbHelp renders a verb's curated --help in the shape every verb shares:
// summary line, prose description, worked examples, then the verb's own flag
// list. The flags are taken straight from fs so they cannot drift from reality.
func printVerbHelp(out io.Writer, v verb, fs *flag.FlagSet) {
	fmt.Fprintf(out, "frothy %s — %s\n\n%s\n\nExamples:\n%s\n\nFlags:\n",
		v.name, v.summary, strings.TrimSpace(v.longDesc), strings.TrimRight(v.examples, "\n"))
	fs.SetOutput(out)
	fs.PrintDefaults()
}

func printFrothyUsage(out io.Writer, verbs []verb) {
	fmt.Fprintln(out, "usage: frothy <verb> [options]")
	fmt.Fprintln(out)
	fmt.Fprintln(out, "commands:")
	printedGroups := make(map[string]bool)
	for _, v := range verbs {
		if printedGroups[v.group] {
			continue
		}
		printedGroups[v.group] = true
		if v.group != "" {
			fmt.Fprintf(out, "\n%s:\n", v.group)
		}
		for _, grouped := range verbs {
			if grouped.group == v.group {
				fmt.Fprintf(out, "  %-10s  %s\n", grouped.name, grouped.summary)
			}
		}
	}
	fmt.Fprintln(out)
	fmt.Fprintln(out, "Run 'frothy <verb> --help' to see the verb's options.")
}

func main() {
	if len(os.Args) > 0 && filepath.Base(os.Args[0]) == "frothy" {
		frothySelfPath = resolveMenuExecutable(os.Args[0])
		if interactiveFrothyMenu(os.Args, os.Stdin, os.Stdout) {
			os.Args = append(os.Args, "menu")
		}
		os.Exit(runFrothyCommand(os.Args, os.Stdout, os.Stderr, availableVerbs()))
	}
	runSessionMain()
}

func runFrothyCommand(args []string, stdout io.Writer, stderr io.Writer, verbs []verb) int {
	if len(args) < 2 {
		printFrothyUsage(stderr, verbs)
		return 2
	}
	// source-plan is internal editor plumbing, not a human CLI workflow.
	if args[1] == "source-plan" {
		return runSourcePlanCommand(args[2:], stdout, stderr)
	}
	switch args[1] {
	case "--help", "-h":
		printFrothyUsage(stdout, verbs)
		return 0
	case "help":
		if len(args) < 3 {
			printFrothyUsage(stdout, verbs)
			return 0
		}
		for _, v := range verbs {
			if v.name == args[2] {
				os.Args = []string{args[0] + " " + v.name, "--help"}
				return v.run()
			}
		}
		fmt.Fprintf(stderr, "no such verb: %s\n", args[2])
		printFrothyUsage(stdout, verbs)
		return 2
	}
	for _, v := range verbs {
		if v.name == args[1] {
			os.Args = append([]string{args[0] + " " + v.name}, args[2:]...)
			return v.run()
		}
	}
	fmt.Fprintf(stderr, "frothy: unknown verb %q\n", args[1])
	printFrothyUsage(stderr, verbs)
	return 2
}

func runSessionMain() int {
	var (
		port       = flag.String("port", "", "serial port, for example /dev/cu.usbmodem101")
		baud       = flag.Int("baud", 115200, "serial baud rate")
		filePath   = flag.String("file", "", "load source lines from a file, applying boot definitions last")
		records    = flag.Bool("records", false, "emit NDJSON session records on stdout")
		transcript = flag.String("transcript", "", "write NDJSON session records to a file; requires --records")
		replay     = flag.String("replay", "", "replay accepted source from an NDJSON record transcript")
		timeout    = flag.Duration("timeout", 3*time.Second, "serial prompt timeout")
		settle     = flag.Duration("settle", 2*time.Second, "delay after opening serial")
	)
	if helpRequested(os.Args[1:]) {
		printVerbHelp(os.Stdout, helpFor("session"), flag.CommandLine)
		return 0
	}
	flag.Parse()

	if exitCode, err := validateSessionOptions(*filePath, *records, *transcript, *replay); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(exitCode)
	}

	input := io.Reader(os.Stdin)
	if *replay != "" {
		lines, err := readReplayLines(*replay)
		if err != nil {
			fmt.Fprintf(os.Stderr, "replay: %v\n", err)
			os.Exit(1)
		}
		input = readerFromLines(lines)
	} else if *filePath != "" {
		lines, err := readFileLines(*filePath)
		if err != nil {
			fmt.Fprintf(os.Stderr, "file: %v\n", err)
			os.Exit(1)
		}
		input = readerFromLines(lines)
	}

	var recordOutput *recordWriter
	if *records {
		output, closeOutput, err := openRecordOutput(os.Stdout, *transcript)
		if err != nil {
			fmt.Fprintf(os.Stderr, "transcript: %v\n", err)
			os.Exit(1)
		}
		defer func() {
			if err := closeOutput(); err != nil {
				fmt.Fprintf(os.Stderr, "transcript: %v\n", err)
			}
		}()

		recordOutput = newRecordWriter(output, newSessionID())
		if err := recordOutput.sessionStart(); err != nil {
			fmt.Fprintf(os.Stderr, "records: %v\n", err)
			os.Exit(1)
		}
	}

	chosen, err := pickSessionPort(*port, defaultPortLister, recordOutput)
	if err != nil {
		fmt.Fprintf(os.Stderr, "session: %v\n", err)
		os.Exit(2)
	}

	dev, err := openSerial(chosen, *baud)
	if err != nil {
		fmt.Fprintf(os.Stderr, "serial: %v\n", err)
		os.Exit(1)
	}
	defer dev.close()
	tracker := &interruptTracker{}
	stopForwardingInterrupts := forwardInterruptSignals(dev, os.Stderr, tracker)
	defer stopForwardingInterrupts()
	time.Sleep(*settle)

	status, err := readDeviceStatus(dev, *timeout)
	if err != nil {
		if recordOutput != nil {
			_ = recordOutput.sessionError(recordStateError, recordErrorStatusFailed, err.Error())
		}
		fmt.Fprintf(os.Stderr, "status: device silent or wedged; %s: %v\n", wipeRecoveryHint(chosen), err)
		os.Exit(1)
	}

	if recordOutput != nil {
		if err := recordOutput.status(status); err != nil {
			fmt.Fprintf(os.Stderr, "records: %v\n", err)
			os.Exit(1)
		}
		if err := runSerialRecords(input, recordOutput, dev, *timeout, tracker); err != nil {
			fmt.Fprintf(os.Stderr, "session: %v\n", err)
			os.Exit(1)
		}
		return 0
	}

	if err := runSerialWithInterrupts(input, os.Stdout, dev, *timeout, tracker); err != nil {
		fmt.Fprintf(os.Stderr, "session: %v\n", err)
		os.Exit(1)
	}
	return 0
}
