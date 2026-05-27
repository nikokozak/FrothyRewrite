//go:build !cgo || (!darwin && !linux)

package testpty

import (
	"errors"
	"os"
)

func Open() (*os.File, *os.File, string, error) {
	return nil, nil, "", errors.New("test pty is only available with cgo on darwin or linux")
}
