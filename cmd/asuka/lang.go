package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/lang/target"
)

// The commands of the Asuka language's compiler (lang/compile): check,
// test, run.

// errReported says the command printed what went wrong already.
var errReported = errors.New("failed")

// cmdCheck reports a package's diagnostics: for every platform's files
// (a package's tray_linux.asuka is Linux's), or --platform's.
func cmdCheck(args []string) error {
	var platform string
	fs := newFlagSet("check", "[dir]")
	fs.about = "Checks the Asuka package in dir (default .): its files, and what they import, for every\n" +
		"platform (the files for some platforms only, tray_linux.asuka, with those of each). Prints\n" +
		"each problem as file:line:col: message, and which platforms it's on when not every one."
	platformFlag(fs, &platform, []string{platformIOS, platformAndroid, platformMac, platformWindows, platformLinux, platformWeb},
		"check for this `platform` only: ios, android, mac, windows, linux or web (default: every one)")
	if err := fs.parse(args); err != nil {
		return err
	}
	dir := "."
	if len(fs.Args()) > 0 {
		dir = fs.Args()[0]
	}
	var ps []target.Platform
	if platform != "" {
		ps = []target.Platform{langPlatform(platform)}
	}
	res, err := compile.DirPlatforms(dir, nil, ps)
	if err != nil {
		return err
	}
	if printPlatformDiags(os.Stderr, res) {
		return errReported
	}
	return nil
}

// printPlatformDiags prints a check's diagnostics for several platforms,
// paths relative to the working directory; it reports whether there were
// any.
func printPlatformDiags(w io.Writer, res *compile.Platforms) bool {
	wd, _ := os.Getwd()
	rel := func(d compile.PlatformDiagnostic) compile.PlatformDiagnostic {
		if r, err := filepath.Rel(wd, d.Pos.Filename); err == nil && !strings.HasPrefix(r, "..") {
			d.Pos.Filename = r
		}
		return d
	}
	for _, d := range res.Warnings {
		d = rel(d)
		d.Msg = "warning: " + d.Msg
		fmt.Fprintln(w, d.String())
	}
	for _, d := range res.Diags {
		fmt.Fprintln(w, rel(d).String())
	}
	return len(res.Diags) > 0
}

// runModule runs a compiled module's tests and prints what they print.
// hide is a test whose "ok" line isn't shown (main's).
func runModule(res *compile.Result, test bool, pattern string, verbose bool, hide string) error {
	runner, err := compile.Runner(false)
	if err != nil {
		return err
	}
	out, err := compile.Run(runner, res.Module, test, pattern)
	failed := false
	for _, line := range strings.Split(strings.TrimRight(out, "\n"), "\n") {
		switch {
		case line == "ok "+hide && hide != "":
		case strings.HasPrefix(line, "ok "):
			if verbose {
				fmt.Println(line)
			}
		case strings.HasPrefix(line, "FAIL "):
			failed = true
			fmt.Println(line)
		case line != "":
			fmt.Println(line)
		}
	}
	if err != nil || failed {
		if !failed {
			return fmt.Errorf("the runner failed: %v", err)
		}
		return errReported
	}
	if !verbose && hide == "" {
		fmt.Printf("ok (%d tests)\n", countTests(res, pattern))
	}
	return nil
}

func countTests(res *compile.Result, pattern string) int {
	n := 0
	for _, t := range res.Module.Tests {
		if pattern == "" || strings.Contains(t.Name, pattern) {
			n++
		}
	}
	return n
}
