package main

import (
	"bufio"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

type menuContext struct {
	canonical bool
	inProject bool
	boards    []string
	repoRoot  string
	selfPath  string
}

type menuVerbRunner func(verb string, args []string, interactive bool) int

const menuBack = -1
const menuEOF = -2
const frothyFlasherURL = "https://frothy.dev/flash/"

var frothySelfPath string

func runMenuMain() int {
	ctx := defaultMenuContext()
	return runMenuCommand(os.Args[1:], os.Stdin, os.Stdout, os.Stderr, ctx,
		ctx.runVerb)
}

func runMenuCommand(args []string, stdin io.Reader, stdout io.Writer,
	stderr io.Writer, ctx menuContext, run menuVerbRunner) int {
	fs := flag.NewFlagSet("frothy menu", flag.ContinueOnError)
	fs.SetOutput(stderr)
	if helpRequested(args) {
		printVerbHelp(stdout, helpFor("menu"), fs)
		return 0
	}
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if fs.NArg() != 0 {
		fmt.Fprintln(stderr, "menu: takes no positional arguments")
		return 2
	}
	if run == nil {
		run = ctx.runVerb
	}
	return runMenu(stdin, stdout, stderr, ctx, run)
}

func runMenu(stdin io.Reader, stdout io.Writer, stderr io.Writer,
	ctx menuContext, run menuVerbRunner) int {
	reader := bufio.NewReader(stdin)

	for {
		printMenuHome(stdout, ctx)
		choice, ok := readMenuChoice(reader, stdout, "> ")
		if !ok {
			fmt.Fprintln(stdout)
			return 0
		}
		switch choice {
		case "q", "quit":
			return 0
		case "1":
			if code := runMenuSetup(reader, stdout, stderr, ctx, run); code != menuBack {
				return code
			}
		case "2":
			return runMenuVerb(stdout, run, "connect", nil, true)
		case "3":
			if code := runMenuRecovery(reader, stdout, stderr, ctx, run); code != menuBack && code != menuEOF {
				return code
			}
		default:
			fmt.Fprintln(stdout, "Choose 1, 2, 3, or q.")
		}
	}
}

func printMenuHome(out io.Writer, ctx menuContext) {
	setup := "Set up a board"
	if ctx.canonical && ctx.inProject {
		setup = "Build and install this project"
	} else if !ctx.canonical {
		setup = "Flash a board in your browser"
	}
	fmt.Fprintln(out, "Frothy")
	fmt.Fprintln(out)
	fmt.Fprintf(out, "  1) %s\n", setup)
	fmt.Fprintln(out, "  2) Connect")
	fmt.Fprintln(out, "  3) Something's wrong")
	fmt.Fprintln(out)
	fmt.Fprintln(out, "  q) quit")
	fmt.Fprintln(out)
}

func runMenuSetup(reader *bufio.Reader, stdout io.Writer, stderr io.Writer,
	ctx menuContext, run menuVerbRunner) int {
	if ctx.canonical && ctx.inProject {
		if code := runMenuVerb(stdout, run, "build", nil, false); code != 0 {
			return recoverAfterMenuFailure(reader, stdout, stderr, ctx, run, code)
		}
		if code := runMenuVerb(stdout, run, "install", nil, false); code != 0 {
			return recoverAfterMenuFailure(reader, stdout, stderr, ctx, run, code)
		}
		return runMenuVerb(stdout, run, "connect", nil, true)
	}

	if !ctx.canonical {
		fmt.Fprintf(stdout, "Open %s in a desktop WebSerial browser.\n", frothyFlasherURL)
		fmt.Fprintln(stdout, "Firmware development commands require a Frothy source checkout.")
		return 0
	}
	if len(ctx.boards) == 0 {
		fmt.Fprintln(stderr, "menu: no boards found under boards/")
		return 2
	}

	_ = runMenuVerb(stdout, run, "doctor", nil, false)
	board, ok := chooseBoard(reader, stdout, ctx.boards)
	if !ok {
		return 0
	}
	if code := runMenuVerb(stdout, run, "flash", []string{board}, false); code != 0 {
		return recoverAfterMenuFailure(reader, stdout, stderr, ctx, run, code)
	}
	return runMenuVerb(stdout, run, "connect", nil, true)
}

func recoverAfterMenuFailure(reader *bufio.Reader, stdout io.Writer, stderr io.Writer,
	ctx menuContext, run menuVerbRunner, failedCode int) int {
	fmt.Fprintln(stdout)
	fmt.Fprintln(stdout, "That command failed. Recovery options:")
	code := runMenuRecovery(reader, stdout, stderr, ctx, run)
	if code == menuEOF {
		return failedCode
	}
	return code
}

func runMenuRecovery(reader *bufio.Reader, stdout io.Writer, stderr io.Writer,
	ctx menuContext, run menuVerbRunner) int {
	for {
		rawWipe := "Board is wedged: erase persisted device state"
		sdkRepair := "Install or repair ESP-IDF"
		if !ctx.canonical {
			rawWipe += " (source checkout required)"
			sdkRepair += " (source checkout required)"
		}
		fmt.Fprintln(stdout, "Something's wrong")
		fmt.Fprintln(stdout)
		fmt.Fprintln(stdout, "  1) Port is busy: stop Frothy serial sessions")
		fmt.Fprintln(stdout, "  2) Clear my user definitions on a running board")
		fmt.Fprintf(stdout, "  3) %s\n", rawWipe)
		fmt.Fprintln(stdout, "  4) Check setup with doctor")
		fmt.Fprintf(stdout, "  5) %s\n", sdkRepair)
		fmt.Fprintln(stdout)
		fmt.Fprintln(stdout, "  b) back")
		fmt.Fprintln(stdout, "  q) quit")
		fmt.Fprintln(stdout)

		choice, ok := readMenuChoice(reader, stdout, "> ")
		if !ok {
			fmt.Fprintln(stdout)
			return menuEOF
		}
		switch choice {
		case "b", "back":
			return menuBack
		case "q", "quit":
			return 0
		case "1":
			return runMenuVerb(stdout, run, "stop", nil, false)
		case "2":
			return runMenuVerb(stdout, run, "wipe-user", nil, false)
		case "3":
			if !ctx.canonical || len(ctx.boards) == 0 {
				fmt.Fprintln(stderr, "menu: raw persistence recovery requires a Frothy source checkout")
				return 2
			}
			board, ok := chooseBoard(reader, stdout, ctx.boards)
			if !ok {
				return 0
			}
			return runMenuVerb(stdout, run, "wipe", []string{board, "--force"}, false)
		case "4":
			return runMenuVerb(stdout, run, "doctor", nil, false)
		case "5":
			if !ctx.canonical {
				fmt.Fprintln(stderr, "menu: ESP-IDF setup requires a Frothy source checkout")
				return 2
			}
			return runMenuVerb(stdout, run, "bootstrap", nil, false)
		default:
			fmt.Fprintln(stdout, "Choose 1, 2, 3, 4, 5, b, or q.")
		}
	}
}

func chooseBoard(reader *bufio.Reader, stdout io.Writer, boards []string) (string, bool) {
	if len(boards) == 1 {
		for {
			fmt.Fprintf(stdout, "Board [%s]: ", boards[0])
			choice, ok := readLine(reader)
			if !ok {
				fmt.Fprintln(stdout)
				return "", false
			}
			choice = strings.TrimSpace(choice)
			if choice == "" || choice == boards[0] {
				return boards[0], true
			}
			if choice == "q" || choice == "quit" || choice == "b" || choice == "back" {
				return "", false
			}
			fmt.Fprintf(stdout, "Choose %s, or q to quit.\n", boards[0])
		}
	}
	for {
		fmt.Fprintln(stdout, "Choose a board:")
		for i, board := range boards {
			fmt.Fprintf(stdout, "  %d) %s\n", i+1, board)
		}
		choice, ok := readMenuChoice(reader, stdout, "> ")
		if !ok {
			fmt.Fprintln(stdout)
			return "", false
		}
		if choice == "q" || choice == "quit" || choice == "b" || choice == "back" {
			return "", false
		}
		for i, board := range boards {
			if choice == fmt.Sprint(i+1) || choice == board {
				return board, true
			}
		}
		fmt.Fprintln(stdout, "Choose one of the listed boards, or q to quit.")
	}
}

func runMenuVerb(stdout io.Writer, run menuVerbRunner, verb string, args []string,
	interactive bool) int {
	fmt.Fprintf(stdout, "→ frothy %s", verb)
	for _, arg := range args {
		fmt.Fprintf(stdout, " %s", arg)
	}
	fmt.Fprintln(stdout)
	return run(verb, args, interactive)
}

func readMenuChoice(reader *bufio.Reader, stdout io.Writer, prompt string) (string, bool) {
	fmt.Fprint(stdout, prompt)
	choice, ok := readLine(reader)
	return strings.TrimSpace(choice), ok
}

func readLine(reader *bufio.Reader) (string, bool) {
	line, err := reader.ReadString('\n')
	if err != nil && len(line) == 0 {
		return "", false
	}
	return strings.TrimRight(line, "\r\n"), true
}

func defaultMenuContext() menuContext {
	wd, _ := os.Getwd()
	root, _ := resolveFrothySourceRoot(wd)
	ctx := menuContext{selfPath: defaultMenuExecutable()}
	if root != "" {
		ctx.canonical = true
		ctx.repoRoot = root
		ctx.boards = listFlashableBoards(filepath.Join(root, "boards"))
	}
	if _, err := os.Stat(filepath.Join(wd, "frothy.toml")); err == nil {
		ctx.inProject = true
	}
	return ctx
}
func defaultMenuExecutable() string {
	if frothySelfPath != "" {
		return frothySelfPath
	}
	path, err := os.Executable()
	if err != nil || path == "" {
		return "frothy"
	}
	return path
}

func resolveMenuExecutable(invoked string) string {
	if invoked == "" {
		return ""
	}
	if strings.ContainsRune(invoked, os.PathSeparator) {
		if abs, err := filepath.Abs(invoked); err == nil {
			return abs
		}
		return invoked
	}
	path, err := exec.LookPath(invoked)
	if err != nil {
		return invoked
	}
	if abs, err := filepath.Abs(path); err == nil {
		return abs
	}
	return path
}

func (ctx menuContext) runVerb(verb string, args []string, interactive bool) int {
	path := ctx.selfPath
	if path == "" {
		path = defaultMenuExecutable()
	}
	cmd := exec.Command(path, append([]string{verb}, args...)...)
	if ctx.canonical && ctx.repoRoot != "" {
		cmd.Dir = ctx.repoRoot
	}
	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		if exit, ok := err.(*exec.ExitError); ok {
			return exit.ExitCode()
		}
		fmt.Fprintf(os.Stderr, "menu: %s: %v\n", verb, err)
		return 1
	}
	_ = interactive
	return 0
}

func interactiveFrothyMenu(args []string, stdin *os.File, stdout *os.File) bool {
	return len(args) < 2 && readerIsTerminal(stdin) && writerIsTerminal(stdout)
}
