//go:build !darwin && !linux && !freebsd && !netbsd && !openbsd && !dragonfly && !windows

package main

import "os"

// rawKeys: no keys on this system.
func rawKeys(f *os.File) (restore func(), ok bool) { return nil, false }
