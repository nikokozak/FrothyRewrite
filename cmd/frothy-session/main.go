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
	"strconv"
	"strings"
	"sync"
	"time"
)

type compileAction int

const (
	actionPass compileAction = iota
	actionApply
	actionSend
	actionClear
	actionError
)

type compileResult struct {
	action compileAction
	line   string
}

type sessionCompiler interface {
	targetProfile() (compilerTarget, error)
	compile(line string) (compileResult, error)
	commit() error
	drop() error
}

type sessionDevice interface {
	syncPrompt(timeout time.Duration) error
	sendLine(line string, timeout time.Duration, promptSeen func()) (string, error)
	interrupt(timeout time.Duration) (string, error)
}

type compilerMode string

const (
	compilerDevice       compilerMode = "device"
	compilerHostRequired compilerMode = "host-required"
	compilerHostOptional compilerMode = "host-optional"
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

type compilerTarget struct {
	profile     string
	profileHash string
	wordSize    uint16
	intMin      int64
	intMax      int64
	applyBytes  uint16
}

var errPromptTimeout = errors.New("timed out waiting for prompt")

const (
	deviceInterruptedStatus  = "err 10"
	promptPrimary            = "frothy> "
	promptContinuation       = ".. "
	shareTerminalInterrupt   = false
	isolateTerminalInterrupt = true
	// Keep this basename in sync with OVERLAY_COMPILER in the Makefile.
	compilerProgramName = "frothy-compile-overlay"
)

func executableFileExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && !info.IsDir() && info.Mode().Perm()&0111 != 0
}

func defaultCompilerPath() string {
	return defaultCompilerPathFrom(os.Executable, filepath.EvalSymlinks, executableFileExists, exec.LookPath)
}

func defaultCompilerPathFrom(
	executable func() (string, error),
	evalSymlinks func(string) (string, error),
	isExecutable func(string) bool,
	lookPath func(string) (string, error),
) string {
	if exe, err := executable(); err == nil {
		if resolved, err := evalSymlinks(exe); err == nil {
			exe = resolved
		}
		for _, candidate := range compilerCandidatesForExecutable(exe) {
			if isExecutable(candidate) {
				return candidate
			}
		}
	}

	if path, err := lookPath(compilerProgramName); err == nil {
		return path
	}

	return ""
}

func compilerCandidatesForExecutable(exe string) []string {
	dir := filepath.Dir(exe)
	return []string{
		filepath.Clean(filepath.Join(dir, "..", "libexec", "frothy", compilerProgramName)),
		filepath.Join(dir, compilerProgramName),
	}
}

type compiler struct {
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	stdout *bufio.Scanner
	stderr *lockedBuffer
}

type lockedBuffer struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *lockedBuffer) Write(data []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(data)
}

func (b *lockedBuffer) Snapshot() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}

func startCompiler(path string, isolateInterrupt bool) (*compiler, error) {
	if path == "" {
		return nil, errors.New("cannot find frothy-compile-overlay; pass --compiler or run make frothy-session")
	}

	cmd := exec.Command(path)
	stderr := &lockedBuffer{}
	cmd.Stderr = stderr
	if isolateInterrupt {
		configureCompilerProcess(cmd)
	}

	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	stdoutPipe, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, err
	}

	scanner := bufio.NewScanner(stdoutPipe)
	scanner.Buffer(make([]byte, 0, 1024), 64*1024)
	return &compiler{
		cmd:    cmd,
		stdin:  stdin,
		stdout: scanner,
		stderr: stderr,
	}, nil
}

func (c *compiler) close() {
	_ = c.stdin.Close()
	_ = c.cmd.Wait()
}

func (c *compiler) request(line string) (string, error) {
	if _, err := fmt.Fprintln(c.stdin, line); err != nil {
		return "", err
	}
	if !c.stdout.Scan() {
		if err := c.stdout.Err(); err != nil {
			return "", err
		}
		if text := strings.TrimSpace(c.stderr.Snapshot()); text != "" {
			return "", errors.New(text)
		}
		return "", errors.New("compiler helper exited")
	}
	return c.stdout.Text(), nil
}

func (c *compiler) targetProfile() (compilerTarget, error) {
	out, err := c.request("@target")
	if err != nil {
		return compilerTarget{}, err
	}
	return parseCompilerTarget(out)
}

func (c *compiler) compile(line string) (compileResult, error) {
	out, err := c.request(line)
	if err != nil {
		return compileResult{}, err
	}
	switch {
	case out == "pass":
		return compileResult{action: actionPass, line: line}, nil
	case strings.HasPrefix(out, "apply "):
		return compileResult{action: actionApply, line: out}, nil
	case strings.HasPrefix(out, "send "):
		return compileResult{action: actionSend, line: strings.TrimPrefix(out, "send ")}, nil
	case out == "clear":
		return compileResult{action: actionClear, line: "clear"}, nil
	case strings.HasPrefix(out, "err "):
		return compileResult{action: actionError, line: out}, nil
	default:
		return compileResult{}, fmt.Errorf("unexpected compiler response %q", out)
	}
}

func (c *compiler) commit() error {
	out, err := c.request("@commit")
	if err != nil {
		return err
	}
	if out != "ok" {
		return fmt.Errorf("compiler commit failed: %s", out)
	}
	return nil
}

func (c *compiler) drop() error {
	out, err := c.request("@drop")
	if err != nil {
		return err
	}
	if out != "ok" {
		return fmt.Errorf("compiler drop failed: %s", out)
	}
	return nil
}

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
		return nil, err
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
	text := strings.ReplaceAll(response, "\r\n", "\n")
	text = strings.ReplaceAll(text, "\r", "\n")
	if strings.HasSuffix(text, "ok\n") {
		return true
	}
	status := responseStatus(response)
	return strings.HasPrefix(status, "err ")
}

func promptComplete(text string, requireStatus bool) bool {
	if !strings.HasSuffix(text, "> ") {
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
	if err := d.writeBytes([]byte(line + "\n")); err != nil {
		return "", err
	}
	return d.readUntilPrompt(timeout, true, promptSeen)
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
	text := strings.ReplaceAll(response, "\r\n", "\n")
	text = strings.ReplaceAll(text, "\r", "\n")
	lines := strings.Split(text, "\n")
	for i := len(lines) - 1; i >= 0; i-- {
		line := strings.TrimSpace(lines[i])
		if line != "" {
			return line
		}
	}
	return ""
}

func responseOK(response string) bool {
	return responseStatus(response) == "ok"
}

func responseSettledAfterInterrupt(response string) bool {
	status := responseStatus(response)
	return status == "ok" || status == deviceInterruptedStatus
}

func promptRecoveredAfterInterrupt(partial string, recovery string) bool {
	return responseSettledAfterInterrupt(recovery) ||
		responseSettledAfterInterrupt(partial+recovery)
}

func affectsCompilerMirror(action compileAction) bool {
	return action == actionApply || action == actionClear
}

func recordAction(action compileAction) string {
	switch action {
	case actionPass:
		return "direct"
	case actionSend:
		return "eval"
	case actionApply:
		return "apply"
	case actionClear:
		return "clear"
	default:
		return "unknown"
	}
}

func compileErrorReason(line string) string {
	if strings.Contains(line, "apply_bytes=") ||
		strings.HasPrefix(strings.TrimSpace(line), "err 4") {
		return "budget"
	}
	return "source"
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
		switch status.compiler {
		case compilerDevice, compilerHostRequired, compilerHostOptional:
		default:
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

func parseCompilerTarget(response string) (compilerTarget, error) {
	fields := strings.Fields(strings.TrimSpace(response))
	if len(fields) < 3 || fields[0] != "target" {
		return compilerTarget{}, fmt.Errorf("malformed compiler target: %s", response)
	}

	values := make(map[string]string)
	for _, field := range fields[1:] {
		key, value, ok := strings.Cut(field, "=")
		if !ok || key == "" || value == "" {
			return compilerTarget{}, fmt.Errorf("malformed compiler target field: %s", field)
		}
		values[key] = value
	}

	applyBytes, err := parseUint16Field(values, "apply_bytes")
	if err != nil {
		return compilerTarget{}, err
	}
	wordSize, err := parseUint16Field(values, "word_size")
	if err != nil {
		return compilerTarget{}, err
	}
	intMin, err := parseInt64Field(values, "int_min")
	if err != nil {
		return compilerTarget{}, err
	}
	intMax, err := parseInt64Field(values, "int_max")
	if err != nil {
		return compilerTarget{}, err
	}

	target := compilerTarget{
		profile:     values["profile"],
		profileHash: values["profile_hash"],
		wordSize:    wordSize,
		intMin:      intMin,
		intMax:      intMax,
		applyBytes:  applyBytes,
	}
	if target.profile == "" || target.profile == "unknown" {
		return compilerTarget{}, errors.New("compiler target missing profile")
	}
	if !validProfileHash(target.profileHash) {
		return compilerTarget{}, errors.New("compiler target missing profile_hash")
	}
	if err := validateIntRange("compiler target", target.wordSize, target.intMin, target.intMax); err != nil {
		return compilerTarget{}, err
	}
	return target, nil
}

func retryableStatusResponse(response string) bool {
	text := strings.ReplaceAll(response, "\r\n", "\n")
	text = strings.ReplaceAll(text, "\r", "\n")
	if strings.Contains(text, "frothy status ") {
		return false
	}

	status := responseStatus(response)
	return status == "" || status == "ok"
}

func verifyCompilerTarget(comp sessionCompiler, status deviceStatus) error {
	if comp == nil {
		return errors.New("host compiler is required by device status")
	}
	target, err := comp.targetProfile()
	if err != nil {
		return err
	}
	if target.profileHash != status.profileHash {
		return fmt.Errorf("compiler target %s/%s does not match device %s/%s",
			target.profile, target.profileHash, status.profile, status.profileHash)
	}
	if target.applyBytes != status.applyBytes {
		return fmt.Errorf("compiler target apply_bytes=%d does not match device apply_bytes=%d",
			target.applyBytes, status.applyBytes)
	}
	if target.wordSize != status.wordSize || target.intMin != status.intMin ||
		target.intMax != status.intMax {
		return fmt.Errorf("compiler target word_size=%d int_range=%d..%d does not match device word_size=%d int_range=%d..%d",
			target.wordSize, target.intMin, target.intMax,
			status.wordSize, status.intMin, status.intMax)
	}
	return nil
}

func readDeviceStatus(dev sessionDevice, timeout time.Duration) (deviceStatus, error) {
	var lastErr error

	if err := dev.syncPrompt(timeout); err != nil {
		return deviceStatus{}, err
	}

	for attempt := 0; attempt < 3; attempt++ {
		response, err := dev.sendLine("status", timeout, nil)
		if err != nil {
			return deviceStatus{}, err
		}

		status, err := parseDeviceStatus(response)
		if err == nil {
			return status, nil
		}
		lastErr = err
		if !retryableStatusResponse(response) || attempt == 2 {
			return deviceStatus{}, err
		}
		if err := dev.syncPrompt(timeout); err != nil {
			return deviceStatus{}, lastErr
		}
	}

	return deviceStatus{}, lastErr
}

func (s deviceStatus) useHostCompiler(hostCompile bool) (bool, error) {
	switch s.compiler {
	case compilerDevice:
		if hostCompile {
			return false, errors.New("device did not advertise host compilation")
		}
		return false, nil
	case compilerHostRequired:
		return true, nil
	case compilerHostOptional:
		return hostCompile, nil
	default:
		return false, fmt.Errorf("unsupported compiler mode: %s", s.compiler)
	}
}

type sourceFormState struct {
	lines        []string
	parenDepth   int
	bracketDepth int
	braceDepth   int
	inString     bool
	escaped      bool
}

func (s *sourceFormState) hasPending() bool {
	return len(s.lines) != 0
}

func (s *sourceFormState) reset() {
	s.lines = nil
	s.parenDepth = 0
	s.bracketDepth = 0
	s.braceDepth = 0
	s.inString = false
	s.escaped = false
}

func (s *sourceFormState) appendLine(line string) (string, bool) {
	trimmed := strings.TrimSpace(line)
	if !s.hasPending() && trimmed == "" {
		return "", false
	}

	s.lines = append(s.lines, trimmed)
	s.scan(line)

	source := s.source()
	if s.parenDepth == 0 && s.bracketDepth == 0 && s.braceDepth == 0 &&
		!s.inString && !sourceNeedsContinuation(source) {
		s.reset()
		return source, true
	}
	return "", false
}

func (s *sourceFormState) source() string {
	parts := make([]string, 0, len(s.lines))
	for _, line := range s.lines {
		if line != "" {
			parts = append(parts, line)
		}
	}
	return strings.Join(parts, " ")
}

func (s *sourceFormState) scan(line string) {
	for i := 0; i < len(line); i++ {
		ch := line[i]
		if s.inString {
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
	}
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
	scanner *bufio.Scanner
	state   sourceFormState
}

func newSourceFormReader(input io.Reader) *sourceFormReader {
	return &sourceFormReader{scanner: bufio.NewScanner(input)}
}

func (r *sourceFormReader) next(prompt io.Writer, interrupts *interruptTracker) (string, bool, error) {
	for {
		if prompt != nil {
			if r.state.hasPending() {
				fmt.Fprint(prompt, promptContinuation)
			} else {
				fmt.Fprint(prompt, promptPrimary)
			}
		}
		if !r.scanner.Scan() {
			if err := r.scanner.Err(); err != nil {
				return "", false, err
			}
			if r.state.hasPending() {
				source := r.state.source()
				r.state.reset()
				return "", false, fmt.Errorf("incomplete top form: %s", source)
			}
			return "", false, nil
		}
		if interrupts != nil && interrupts.consumeIdle() {
			r.state.reset()
		}

		if source, complete := r.state.appendLine(r.scanner.Text()); complete {
			return source, true, nil
		}
	}
}

func collectSourceForms(input io.Reader) ([]string, error) {
	reader := newSourceFormReader(input)
	var forms []string
	for {
		source, ok, err := reader.next(nil, nil)
		if err != nil {
			return nil, err
		}
		if !ok {
			return forms, nil
		}
		forms = append(forms, source)
	}
}

func isBootDefinition(line string) bool {
	fields := strings.Fields(line)
	return len(fields) >= 2 && fields[0] == "boot" && fields[1] == "is"
}

func readFileLines(path string) ([]string, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()

	forms, err := collectSourceForms(file)
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

func validateSessionOptions(filePath string, dryRun bool, records bool, transcript string, replay string) (int, error) {
	if records && dryRun {
		return 2, errors.New("--records cannot be combined with --dry-run")
	}
	if transcript != "" && !records {
		return 2, errors.New("--transcript requires --records")
	}
	if replay != "" && filePath != "" {
		return 2, errors.New("--replay cannot be combined with --file")
	}
	if replay != "" && dryRun {
		return 2, errors.New("--replay cannot be combined with --dry-run")
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
	recordErrorHelperFailed    = "helper_failed"
	recordErrorDeviceLost      = "device_lost"
	recordErrorInterruptFailed = "interrupt_failed"
	recordErrorMirrorStale     = "mirror_stale"
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

func initialMirror(useCompiler bool) recordMirror {
	if useCompiler {
		return recordMirrorClean
	}
	return recordMirrorNone
}

func pendingMirrorForAction(action compileAction, mirror recordMirror) recordMirror {
	if affectsCompilerMirror(action) {
		return recordMirrorPending
	}
	return mirror
}

func selectedMode(status deviceStatus, useCompiler bool) string {
	if useCompiler {
		return string(status.compiler)
	}
	return string(compilerDevice)
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

func (w *recordWriter) status(status deviceStatus, useCompiler bool) error {
	return w.write(recordStatus, recordStateIdle, initialMirror(useCompiler), map[string]any{
		"mode":   selectedMode(status, useCompiler),
		"device": deviceStatusFields(status),
	})
}

func (w *recordWriter) send(source string, result compileResult, mirror recordMirror) error {
	return w.write(recordSend, recordStateWaiting, pendingMirrorForAction(result.action, mirror), map[string]any{
		"source": source,
		"line":   result.line,
		"action": recordAction(result.action),
	})
}

func (w *recordWriter) compileError(source string, result compileResult, mirror recordMirror) error {
	text := result.line
	if text != "" && !strings.HasSuffix(text, "\n") {
		text += "\n"
	}
	return w.write(recordCompileError, recordStateIdle, mirror, map[string]any{
		"source": source,
		"reason": compileErrorReason(result.line),
		"status": responseStatus(text),
		"text":   text,
	})
}

func (w *recordWriter) response(response string, mirror recordMirror, mirrorAction string) error {
	fields := map[string]any{
		"status": responseStatus(response),
		"ok":     responseOK(response),
		"text":   response,
	}
	if mirrorAction != "" {
		fields["mirror_action"] = mirrorAction
	}
	return w.write(recordResponse, recordStateIdle, mirror, fields)
}

func (w *recordWriter) interrupt(state recordState, mirror recordMirror, settled bool, text string, code string) error {
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
	return w.write(recordInterrupt, state, mirror, fields)
}

func (w *recordWriter) sessionError(state recordState, mirror recordMirror, code string, message string) error {
	return w.write(recordSessionError, state, mirror, map[string]any{
		"code":    code,
		"message": message,
	})
}

func (w *recordWriter) sessionEnd(mirror recordMirror) error {
	return w.write(recordSessionEnd, recordStateClosed, mirror, nil)
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

func runDry(input io.Reader, output io.Writer, comp sessionCompiler) error {
	reader := newSourceFormReader(input)
	for {
		source, ok, err := reader.next(nil, nil)
		if err != nil {
			return err
		}
		if !ok {
			return nil
		}

		result, err := comp.compile(source)
		if err != nil {
			return err
		}
		switch result.action {
		case actionPass:
			fmt.Fprintln(output, result.line)
		case actionApply:
			fmt.Fprintln(output, result.line)
			if err := comp.commit(); err != nil {
				return err
			}
		case actionClear:
			fmt.Fprintln(output, result.line)
			if err := comp.commit(); err != nil {
				return err
			}
		case actionSend:
			fmt.Fprintln(output, result.line)
		case actionError:
			fmt.Fprintln(output, result.line)
		}
	}
}

func runSerial(input io.Reader, output io.Writer, comp sessionCompiler, dev sessionDevice, timeout time.Duration) error {
	status, err := readDeviceStatus(dev, timeout)
	if err != nil {
		return err
	}
	useCompiler, err := status.useHostCompiler(false)
	if err != nil {
		return err
	}
	if useCompiler {
		if err := verifyCompilerTarget(comp, status); err != nil {
			return err
		}
	}
	return runSerialWithMode(input, output, comp, dev, timeout, useCompiler)
}

func runSerialWithMode(input io.Reader, output io.Writer, comp sessionCompiler, dev sessionDevice, timeout time.Duration, useCompiler bool) error {
	return runSerialWithModeAndInterrupts(input, output, comp, dev, timeout, useCompiler, nil)
}

func runSerialWithModeAndInterrupts(input io.Reader, output io.Writer, comp sessionCompiler, dev sessionDevice, timeout time.Duration, useCompiler bool, interrupts *interruptTracker) error {
	if useCompiler && comp == nil {
		return errors.New("host compiler is required by device status")
	}

	reader := newSourceFormReader(input)
	for {
		source, ok, err := reader.next(output, interrupts)
		if err != nil {
			return err
		}
		if !ok {
			return nil
		}

		result := compileResult{action: actionPass, line: source}
		if useCompiler {
			compiled, err := comp.compile(source)
			if err != nil {
				return err
			}
			result = compiled
		}
		if result.action == actionError {
			fmt.Fprintln(output, result.line)
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
		response, err := dev.sendLine(result.line, timeout, onPromptSeen)
		if interrupts != nil && !promptSeen {
			interrupted = interrupts.consume()
		}
		if err != nil {
			if errors.Is(err, errPromptTimeout) {
				if err := handlePromptTimeout(output, comp, dev, timeout, result, response); err != nil {
					return err
				}
				continue
			}
			if affectsCompilerMirror(result.action) {
				if comp != nil {
					_ = comp.drop()
				}
			}
			return err
		}
		printDeviceResponse(output, response)
		if interrupted {
			if err := handleSignalInterrupt(comp, result, response); err != nil {
				return err
			}
			continue
		}

		if affectsCompilerMirror(result.action) {
			if responseOK(response) {
				if err := comp.commit(); err != nil {
					return err
				}
			} else if err := comp.drop(); err != nil {
				return err
			}
		}
	}
}

func handleSignalInterrupt(comp sessionCompiler, result compileResult, response string) error {
	if affectsCompilerMirror(result.action) {
		if comp != nil {
			_ = comp.drop()
		}
		return errors.New("device update interrupted; compiler mirror stale")
	}
	if !responseSettledAfterInterrupt(response) {
		return fmt.Errorf("interrupt recovery returned %q; session state unknown", responseStatus(response))
	}
	return nil
}

func runSerialRecordsWithMode(input io.Reader, records *recordWriter, comp sessionCompiler, dev sessionDevice, timeout time.Duration, useCompiler bool, interrupts *interruptTracker) error {
	if interrupts == nil {
		return errors.New("record session requires interrupt tracker")
	}
	if useCompiler && comp == nil {
		err := errors.New("host compiler is required by device status")
		_ = records.sessionError(recordStateError, initialMirror(useCompiler), recordErrorHelperFailed, err.Error())
		return err
	}

	mirror := initialMirror(useCompiler)
	reader := newSourceFormReader(input)
	for {
		source, ok, err := reader.next(nil, interrupts)
		if err != nil {
			_ = records.sessionError(recordStateError, mirror, recordErrorDeviceLost, err.Error())
			return err
		}
		if !ok {
			return records.sessionEnd(mirror)
		}

		result := compileResult{action: actionPass, line: source}
		if useCompiler {
			compiled, err := comp.compile(source)
			if err != nil {
				_ = records.sessionError(recordStateError, mirror, recordErrorHelperFailed, err.Error())
				return err
			}
			result = compiled
		}
		if result.action == actionError {
			if err := records.compileError(source, result, mirror); err != nil {
				return err
			}
			continue
		}

		if err := records.send(source, result, mirror); err != nil {
			return err
		}

		interrupted := false
		promptSeen := false
		interrupts.arm()
		response, err := dev.sendLine(result.line, timeout, func() {
			interrupted = interrupts.commandPromptSeen()
			promptSeen = true
		})
		if !promptSeen {
			interrupted = interrupts.consume()
		}
		if err != nil {
			if errors.Is(err, errPromptTimeout) {
				if err := handleRecordPromptTimeout(records, comp, dev, timeout, result, response, mirror); err != nil {
					return err
				}
				continue
			}
			return handleRecordDeviceError(records, comp, result, mirror, err)
		}
		if interrupted {
			if err := handleRecordSignalInterrupt(records, comp, result, response, mirror); err != nil {
				return err
			}
			continue
		}

		nextMirror, mirrorAction, err := finishRecordMirror(comp, result, response, mirror)
		if err != nil {
			_ = records.sessionError(recordStateStale, recordMirrorStale, recordErrorMirrorStale, err.Error())
			return err
		}
		mirror = nextMirror
		if err := records.response(response, mirror, mirrorAction); err != nil {
			return err
		}
	}
}

func finishRecordMirror(comp sessionCompiler, result compileResult, response string, mirror recordMirror) (recordMirror, string, error) {
	if !affectsCompilerMirror(result.action) {
		return mirror, "", nil
	}
	if responseOK(response) {
		if err := comp.commit(); err != nil {
			return recordMirrorStale, "", err
		}
		return recordMirrorClean, "commit", nil
	}
	if err := comp.drop(); err != nil {
		return recordMirrorStale, "", err
	}
	return recordMirrorClean, "drop", nil
}

func handleRecordDeviceError(records *recordWriter, comp sessionCompiler, result compileResult, mirror recordMirror, err error) error {
	if affectsCompilerMirror(result.action) {
		if comp != nil {
			_ = comp.drop()
		}
		_ = records.sessionError(recordStateStale, recordMirrorStale, recordErrorMirrorStale, err.Error())
		return err
	}
	_ = records.sessionError(recordStateError, mirror, recordErrorDeviceLost, err.Error())
	return err
}

func handleRecordPromptTimeout(records *recordWriter, comp sessionCompiler, dev sessionDevice, timeout time.Duration, result compileResult, partial string, mirror recordMirror) error {
	recovery, recoverErr := dev.interrupt(timeout)
	text := partial + recovery
	settled := recoverErr == nil && promptRecoveredAfterInterrupt(partial, recovery)

	if affectsCompilerMirror(result.action) {
		if comp != nil {
			_ = comp.drop()
		}
		_ = records.interrupt(recordStateStale, recordMirrorStale, settled, text, "")
		err := errors.New("device update timed out; compiler mirror stale")
		if recoverErr != nil {
			err = fmt.Errorf("device update timed out; compiler mirror stale; interrupt recovery failed: %w", recoverErr)
		}
		_ = records.sessionError(recordStateStale, recordMirrorStale, recordErrorMirrorStale, err.Error())
		return err
	}

	if recoverErr != nil {
		err := fmt.Errorf("interrupt recovery failed: %w", recoverErr)
		_ = records.interrupt(recordStateError, mirror, false, text, recordErrorInterruptFailed)
		_ = records.sessionError(recordStateError, mirror, recordErrorInterruptFailed, err.Error())
		return err
	}
	if !settled {
		err := fmt.Errorf("interrupt recovery returned %q; session state unknown", responseStatus(text))
		_ = records.interrupt(recordStateError, mirror, false, text, recordErrorInterruptFailed)
		_ = records.sessionError(recordStateError, mirror, recordErrorInterruptFailed, err.Error())
		return err
	}
	return records.interrupt(recordStateIdle, mirror, true, text, "")
}

func handleRecordSignalInterrupt(records *recordWriter, comp sessionCompiler, result compileResult, response string, mirror recordMirror) error {
	settled := responseSettledAfterInterrupt(response)
	if affectsCompilerMirror(result.action) {
		if comp != nil {
			_ = comp.drop()
		}
		_ = records.interrupt(recordStateStale, recordMirrorStale, settled, response, "")
		err := errors.New("device update interrupted; compiler mirror stale")
		_ = records.sessionError(recordStateStale, recordMirrorStale, recordErrorMirrorStale, err.Error())
		return err
	}
	if !settled {
		err := fmt.Errorf("interrupt recovery returned %q; session state unknown", responseStatus(response))
		_ = records.interrupt(recordStateError, mirror, false, response, recordErrorInterruptFailed)
		_ = records.sessionError(recordStateError, mirror, recordErrorInterruptFailed, err.Error())
		return err
	}
	return records.interrupt(recordStateIdle, mirror, true, response, "")
}

func printDeviceResponse(output io.Writer, response string) {
	fmt.Fprint(output, response)
	if response != "" && !strings.HasSuffix(response, "\n") {
		fmt.Fprintln(output)
	}
}

func handlePromptTimeout(output io.Writer, comp sessionCompiler, dev sessionDevice, timeout time.Duration, result compileResult, partial string) error {
	recovery, recoverErr := dev.interrupt(timeout)
	recoveredResponse := partial + recovery
	printDeviceResponse(output, recoveredResponse)

	if affectsCompilerMirror(result.action) {
		if comp != nil {
			_ = comp.drop()
		}
		if recoverErr != nil {
			return fmt.Errorf("device update timed out; compiler mirror stale; interrupt recovery failed: %w", recoverErr)
		}
		return errors.New("device update timed out; compiler mirror stale")
	}

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

type verb struct {
	name    string
	summary string
	run     func() int
}

func availableVerbs() []verb {
	return []verb{
		{name: "session", summary: "open an interactive REPL session over serial", run: runSessionMain},
		{name: "send", summary: "compile a source file and apply or run each line", run: runSendMain},
	}
}

type sendCompilerFactory func(path string) (sessionCompiler, func(), error)

func defaultSendCompilerFactory(path string) (sessionCompiler, func(), error) {
	c, err := startCompiler(path, shareTerminalInterrupt)
	if err != nil {
		return nil, nil, err
	}
	return c, c.close, nil
}

func runSendMain() int {
	return runSendCommand(os.Args[1:], os.Stdout, os.Stderr, defaultSendCompilerFactory)
}

func runSendCommand(args []string, stdout io.Writer, stderr io.Writer, newCompiler sendCompilerFactory) int {
	fs := flag.NewFlagSet("frothy send", flag.ContinueOnError)
	fs.SetOutput(stderr)
	var (
		dryRun       = fs.Bool("dry-run", false, "compile lines without opening serial")
		port         = fs.String("port", "", "serial port, for example /dev/cu.usbmodem101")
		baud         = fs.Int("baud", 115200, "serial baud rate")
		compilerPath = fs.String("compiler", defaultCompilerPath(), "C overlay compiler helper")
		hostCompile  = fs.Bool("host-compile", false, "use host compiler when the device advertises host-optional mode")
		timeout      = fs.Duration("timeout", 3*time.Second, "serial prompt timeout")
		settle       = fs.Duration("settle", 2*time.Second, "delay after opening serial")
	)
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	rest := fs.Args()
	if len(rest) != 1 {
		fmt.Fprintln(stderr, "send: expected exactly one source file")
		return 2
	}
	path := rest[0]

	if !*dryRun && *port == "" {
		fmt.Fprintln(stderr, "send: --port is required unless --dry-run is set")
		return 2
	}

	lines, err := readFileLines(path)
	if err != nil {
		fmt.Fprintf(stderr, "send: %v\n", err)
		return 1
	}

	if *dryRun {
		comp, closeComp, err := newCompiler(*compilerPath)
		if err != nil {
			fmt.Fprintf(stderr, "compiler: %v\n", err)
			return 1
		}
		defer closeComp()
		if err := runDry(readerFromLines(lines), stdout, comp); err != nil {
			fmt.Fprintf(stderr, "send: %v\n", err)
			return 1
		}
		return 0
	}

	// Reuse session's --file path so the compile+apply engine is not forked.
	sessionArgs := []string{os.Args[0], "--file", path, "--port", *port,
		"--baud", strconv.Itoa(*baud), "--compiler", *compilerPath,
		"--timeout", timeout.String(), "--settle", settle.String()}
	if *hostCompile {
		sessionArgs = append(sessionArgs, "--host-compile")
	}
	oldArgs := os.Args
	os.Args = sessionArgs
	defer func() { os.Args = oldArgs }()
	oldFlag := flag.CommandLine
	flag.CommandLine = flag.NewFlagSet(sessionArgs[0], flag.ExitOnError)
	defer func() { flag.CommandLine = oldFlag }()
	return runSessionMain()
}

func printFrothyUsage(out io.Writer, verbs []verb) {
	fmt.Fprintln(out, "usage: frothy <verb> [options]")
	fmt.Fprintln(out)
	fmt.Fprintln(out, "verbs:")
	for _, v := range verbs {
		fmt.Fprintf(out, "  %-8s  %s\n", v.name, v.summary)
	}
	fmt.Fprintln(out)
	fmt.Fprintln(out, "Run 'frothy <verb> --help' to see the verb's options.")
}

func main() {
	if len(os.Args) > 0 && filepath.Base(os.Args[0]) == "frothy" {
		os.Exit(runFrothyCommand(os.Args, os.Stdout, os.Stderr, availableVerbs()))
	}
	runSessionMain()
}

func runFrothyCommand(args []string, stdout io.Writer, stderr io.Writer, verbs []verb) int {
	if len(args) < 2 {
		printFrothyUsage(stderr, verbs)
		return 2
	}
	switch args[1] {
	case "help", "--help", "-h":
		printFrothyUsage(stdout, verbs)
		return 0
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
		port         = flag.String("port", "", "serial port, for example /dev/cu.usbmodem101")
		baud         = flag.Int("baud", 115200, "serial baud rate")
		compilerPath = flag.String("compiler", defaultCompilerPath(), "C overlay compiler helper")
		dryRun       = flag.Bool("dry-run", false, "print device lines without opening serial")
		filePath     = flag.String("file", "", "load source lines from a file, applying boot definitions last")
		hostCompile  = flag.Bool("host-compile", false, "use host compiler when the device advertises host-optional mode")
		records      = flag.Bool("records", false, "emit NDJSON session records on stdout")
		transcript   = flag.String("transcript", "", "write NDJSON session records to a file; requires --records")
		replay       = flag.String("replay", "", "replay accepted source from an NDJSON record transcript")
		timeout      = flag.Duration("timeout", 3*time.Second, "serial prompt timeout")
		settle       = flag.Duration("settle", 2*time.Second, "delay after opening serial")
	)
	flag.Parse()

	if exitCode, err := validateSessionOptions(*filePath, *dryRun, *records, *transcript, *replay); err != nil {
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

	if *dryRun {
		comp, err := startCompiler(*compilerPath, shareTerminalInterrupt)
		if err != nil {
			fmt.Fprintf(os.Stderr, "compiler: %v\n", err)
			os.Exit(1)
		}
		defer comp.close()

		if err := runDry(input, os.Stdout, comp); err != nil {
			fmt.Fprintf(os.Stderr, "session: %v\n", err)
			os.Exit(1)
		}
		return 0
	}

	if *port == "" {
		fmt.Fprintln(os.Stderr, "--port is required unless --dry-run is set")
		os.Exit(2)
	}

	dev, err := openSerial(*port, *baud)
	if err != nil {
		fmt.Fprintf(os.Stderr, "serial: %v\n", err)
		os.Exit(1)
	}
	defer dev.close()
	tracker := &interruptTracker{}
	stopForwardingInterrupts := forwardInterruptSignals(dev, os.Stderr, tracker)
	defer stopForwardingInterrupts()
	time.Sleep(*settle)

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

	status, err := readDeviceStatus(dev, *timeout)
	if err != nil {
		if recordOutput != nil {
			_ = recordOutput.sessionError(recordStateError, recordMirrorNone, recordErrorStatusFailed, err.Error())
		}
		fmt.Fprintf(os.Stderr, "status: %v\n", err)
		os.Exit(1)
	}

	useCompiler, err := status.useHostCompiler(*hostCompile)
	if err != nil {
		if recordOutput != nil {
			_ = recordOutput.sessionError(recordStateError, recordMirrorNone, recordErrorStatusFailed, err.Error())
		}
		fmt.Fprintf(os.Stderr, "status: %v\n", err)
		os.Exit(1)
	}

	var comp sessionCompiler
	if useCompiler {
		started, err := startCompiler(*compilerPath, isolateTerminalInterrupt)
		if err != nil {
			if recordOutput != nil {
				_ = recordOutput.sessionError(recordStateError, recordMirrorClean, recordErrorHelperFailed, err.Error())
			}
			fmt.Fprintf(os.Stderr, "compiler: %v\n", err)
			os.Exit(1)
		}
		defer started.close()
		if err := verifyCompilerTarget(started, status); err != nil {
			if recordOutput != nil {
				_ = recordOutput.sessionError(recordStateError, recordMirrorClean, recordErrorHelperFailed, err.Error())
			}
			fmt.Fprintf(os.Stderr, "compiler: %v\n", err)
			os.Exit(1)
		}
		comp = started
	}

	if recordOutput != nil {
		if err := recordOutput.status(status, useCompiler); err != nil {
			fmt.Fprintf(os.Stderr, "records: %v\n", err)
			os.Exit(1)
		}
		if err := runSerialRecordsWithMode(input, recordOutput, comp, dev, *timeout, useCompiler, tracker); err != nil {
			fmt.Fprintf(os.Stderr, "session: %v\n", err)
			os.Exit(1)
		}
		return 0
	}

	if err := runSerialWithModeAndInterrupts(input, os.Stdout, comp, dev, *timeout, useCompiler, tracker); err != nil {
		fmt.Fprintf(os.Stderr, "session: %v\n", err)
		os.Exit(1)
	}
	return 0
}
