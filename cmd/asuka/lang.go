package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"strings"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/lang/diag"
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
		"each problem as file:line:col: error[code]: message, then its help and its page (asuka\n" +
		"explain code prints the page), and which platforms it's on when not every one."
	platformFlag(fs, &platform, []string{platformIOS, platformAndroid, platformMac, platformWindows, platformLinux, platformWeb},
		"check for this `platform` only: ios, android, mac, windows, linux or web (default: every one)")
	fs.Bool(&jsonOut, "json", 0, "write each diagnostic as JSON, one object per line on stdout: code, severity, file, range, message, help, url (and platforms, when not every one's)")
	variantFlags(fs)
	if err := fs.parse(args); err != nil {
		return err
	}
	if err := useVariant(); err != nil {
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
	res, err := compile.DirPlatforms(dir, &compile.Options{Variant: appbuild.Variant, Defines: appbuild.Defines}, ps)
	if err != nil {
		return err
	}
	w := io.Writer(os.Stderr)
	if jsonOut {
		w = os.Stdout
	}
	if printPlatformDiags(w, res, jsonOut) {
		return errReported
	}
	return nil
}

// printPlatformDiags prints a check's diagnostics for several platforms,
// paths relative to the working directory, as the terminal says them or
// as JSON lines; it reports whether there were errors.
func printPlatformDiags(w io.Writer, res *compile.Platforms, asJSON bool) bool {
	wd, _ := os.Getwd()
	print := func(d compile.PlatformDiagnostic, sev diag.Severity) {
		d.Diagnostic = relDiag(wd, d.Diagnostic)
		if asJSON {
			r := d.Record(sev)
			for _, p := range d.Platforms {
				r.Platforms = append(r.Platforms, string(p))
			}
			fmt.Fprintln(w, r.JSON())
			return
		}
		after := ""
		if d.Platforms != nil {
			after = "(for " + target.List(d.Platforms) + ")"
		}
		fmt.Fprintln(w, d.Format(sev, after))
	}
	for _, d := range res.Warnings {
		print(d, diag.Warning)
	}
	for _, d := range res.Diags {
		print(d, d.Severity())
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
