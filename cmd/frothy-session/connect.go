package main

// runConnectMain is the connect verb entry. The platform-specific
// path lives in connect_unix.go and connect_windows.go behind build
// tags; this file holds the cross-platform dispatch only.
func runConnectMain() int {
	return runConnect()
}
