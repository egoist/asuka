//go:build unix

package main

import (
	"time"

	"golang.org/x/sys/unix"
)

// lchtimes dates the link p itself, not what it names.
func lchtimes(p string, t time.Time) error {
	tv := unix.NsecToTimeval(t.UnixNano())
	return unix.Lutimes(p, []unix.Timeval{tv, tv})
}
