//go:build cgo && (darwin || linux)

package testpty

/*
#cgo linux LDFLAGS: -lutil
#if defined(__APPLE__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#endif
*/
import "C"

import (
	"fmt"
	"os"
)

func Open() (*os.File, *os.File, string, error) {
	var master C.int
	var slave C.int
	name := make([]C.char, 128)
	rc, err := C.openpty(&master, &slave, &name[0], nil, nil)
	if rc != 0 {
		return nil, nil, "", fmt.Errorf("openpty: %w", err)
	}

	masterFile := os.NewFile(uintptr(master), "pty-master")
	slaveFile := os.NewFile(uintptr(slave), "pty-slave")
	if masterFile == nil || slaveFile == nil {
		return nil, nil, "", fmt.Errorf("openpty returned invalid file descriptors")
	}
	return masterFile, slaveFile, C.GoString(&name[0]), nil
}
