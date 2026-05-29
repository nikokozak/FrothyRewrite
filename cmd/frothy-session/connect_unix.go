//go:build darwin || linux

package main

import "os"

func runConnect() int {
	return runConnectCommand(os.Args[1:], os.Stderr, defaultPortLister, defaultConnectDeviceFactory)
}

func defaultConnectDeviceFactory(port string, baud int) (sessionDevice, func(), error) {
	dev, err := openSerial(port, baud)
	if err != nil {
		return nil, nil, err
	}
	return dev, func() { dev.close() }, nil
}
