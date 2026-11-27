//go:build !windows

package main

import "syscall"

// cmdLine is Windows' (a process's command line as it is); elsewhere
// processes take their arguments one by one.
func cmdLine(line string) *syscall.SysProcAttr { return nil }
