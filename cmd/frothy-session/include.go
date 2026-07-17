package main

import (
	"fmt"
	"path/filepath"
	"strings"
)

// SPEC D3. `include "X"` is a host-side preprocessor directive. The
// preprocessor reads the referenced file (path relative to the file
// containing the directive), splices its contents in place, and
// continues. Cycles fail before compile.
//
// load resolves a path to its source. Tests pass an in-memory map;
// frothy build wraps os.ReadFile.
//
// Cycle detection uses syntactic paths (filepath.Clean). Two different
// paths to the same physical file via symlinks are treated as
// independent.

func preprocessInclude(rootPath string, load func(string) (string, error)) (string, error) {
	return preprocessIncludeAt(filepath.Clean(rootPath), load, nil)
}

func preprocessIncludeAt(path string, load func(string) (string, error), stack []string) (string, error) {
	for _, p := range stack {
		if p == path {
			chain := append(append([]string{}, stack...), path)
			return "", fmt.Errorf("include cycle: %s", strings.Join(chain, " -> "))
		}
	}
	src, err := load(path)
	if err != nil {
		return "", err
	}
	stack = append(stack, path)
	dir := filepath.Dir(path)
	var b strings.Builder
	inBlockComment := false
	for _, line := range strings.SplitAfter(src, "\n") {
		directiveLine := stripFrothyComments(line, &inBlockComment)
		target, ok := matchInclude(directiveLine)
		if !ok {
			b.WriteString(line)
			continue
		}
		if filepath.IsAbs(target) {
			return "", fmt.Errorf("%s: absolute include path %q is not allowed", path, target)
		}
		resolved := filepath.Clean(filepath.Join(dir, target))
		inner, err := preprocessIncludeAt(resolved, load, stack)
		if err != nil {
			return "", err
		}
		b.WriteString(inner)
		if !strings.HasSuffix(inner, "\n") {
			b.WriteString("\n")
		}
	}
	return b.String(), nil
}

func frothyCommentCanStart(line string, i int) bool {
	if i == 0 {
		return true
	}
	prev := line[i-1]
	return prev == ' ' || prev == '\t' || prev == '\n' || prev == '\r' ||
		prev == '[' || prev == '(' || prev == ':' || prev == ',' || prev == ';'
}

func stripFrothyComments(line string, inBlockComment *bool) string {
	var b strings.Builder
	inString := false
	escaped := false

	for i := 0; i < len(line); i++ {
		ch := line[i]
		if *inBlockComment {
			if ch == '*' && i+1 < len(line) && line[i+1] == '-' {
				*inBlockComment = false
				i++
			}
			continue
		}
		if inString {
			b.WriteByte(ch)
			if escaped {
				escaped = false
				continue
			}
			if ch == '\\' {
				escaped = true
				continue
			}
			if ch == '"' {
				inString = false
			}
			continue
		}
		if ch == '"' {
			inString = true
			b.WriteByte(ch)
			continue
		}
		if ch == '-' && i+1 < len(line) && frothyCommentCanStart(line, i) {
			if line[i+1] == '-' {
				break
			}
			if line[i+1] == '*' {
				*inBlockComment = true
				i++
				continue
			}
		}
		b.WriteByte(ch)
	}
	return b.String()
}

// One line, one directive: `include "<path>"` with optional surrounding
// whitespace. Anything else passes through verbatim so the Frothy
// parser handles it. Refusing malformed directives here would compete
// with the parser's error reporting; the splice runs only when the
// shape is unambiguous.
func matchInclude(line string) (string, bool) {
	inBlockComment := false
	line = stripFrothyComments(line, &inBlockComment)
	s := strings.TrimSpace(line)
	const kw = "include"
	if !strings.HasPrefix(s, kw) {
		return "", false
	}
	rest := s[len(kw):]
	if rest == "" || (rest[0] != ' ' && rest[0] != '\t') {
		return "", false
	}
	rest = strings.TrimLeft(rest, " \t")
	if len(rest) < 2 || rest[0] != '"' {
		return "", false
	}
	end := strings.IndexByte(rest[1:], '"')
	if end < 0 {
		return "", false
	}
	target := rest[1 : 1+end]
	if target == "" {
		return "", false
	}
	tail := strings.TrimSpace(rest[1+end+1:])
	if tail != "" {
		return "", false
	}
	return target, true
}
