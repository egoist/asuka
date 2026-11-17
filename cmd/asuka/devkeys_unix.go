//go:build darwin || linux || freebsd || netbsd || openbsd || dragonfly

package main

import (
	"os"

	"golang.org/x/sys/unix"
)

// rawKeys turns f's terminal's line editing and echo off, keeping its
// signals (Ctrl-C) and its output's processing (newlines), and says how
// to turn them back on; false when f isn't a terminal.
func rawKeys(f *os.File) (restore func(), ok bool) {
	fd := int(f.Fd())
	old, err := unix.IoctlGetTermios(fd, ioctlGetTermios)
	if err != nil {
		return nil, false
	}
	t := *old
	t.Lflag &^= unix.ICANON | unix.ECHO
	t.Cc[unix.VMIN], t.Cc[unix.VTIME] = 1, 0
	if err := unix.IoctlSetTermios(fd, ioctlSetTermios, &t); err != nil {
		return nil, false
	}
	return func() { unix.IoctlSetTermios(fd, ioctlSetTermios, old) }, true
}
