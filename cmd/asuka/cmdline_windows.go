package main

import "syscall"

// cmdLine has a process started with line as its command line, as it is:
// cmd.exe parses its own (/s /c "..."), not as Go quotes arguments.
func cmdLine(line string) *syscall.SysProcAttr { return &syscall.SysProcAttr{CmdLine: line} }
