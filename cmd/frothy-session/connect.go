package main

import (
	"fmt"
	"os"
)

// runConnectMain is the connect verb entry. The real Unix line
// editor lands in connect_unix.go; the Windows error path lands in
// connect_windows.go. Until those arrive, the verb registers but
// reports unimplemented so the dispatcher path is exercised.
func runConnectMain() int {
	fmt.Fprintln(os.Stderr, "frothy connect: not yet implemented")
	return 1
}
