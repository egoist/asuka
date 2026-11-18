package main

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	"github.com/egoist/asuka/internal/cachedir"
	"github.com/egoist/asuka/internal/selfupdate"
)

// cmdUpgrade is asuka upgrade: this CLI replaced by its latest release
// (or the one named), from GitHub, checked against the release's
// SHA256SUMS. (asuka update is an app's updates'.)
func cmdUpgrade(args []string) error {
	fs := newFlagSet("upgrade", "[version]")
	fs.about = "Replaces this asuka with its latest release's, or version's (0.3.0), downloaded from\n" +
		"GitHub and checked against the release's SHA256SUMS. Once a day asuka checks for a\n" +
		"newer release on its own and says so (not in CI, or with ASUKA_NO_UPDATE_CHECK=1)."
	if err := fs.parse(args); err != nil {
		return err
	}
	if len(fs.Args()) > 1 {
		return fmt.Errorf("asuka upgrade: one version at most")
	}
	if !selfupdate.Released(version) {
		return fmt.Errorf("this asuka is built from source (version %q), not a release: update its checkout instead", version)
	}
	if _, err := selfupdate.Asset(runtime.GOOS, runtime.GOARCH); err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Minute)
	defer cancel()
	target := strings.TrimPrefix(fs.Arg(0), "v")
	if target == "" {
		latest, err := selfupdate.Latest(ctx)
		if err != nil {
			return fmt.Errorf("the latest release: %v", err)
		}
		if !selfupdate.Newer(latest, version) {
			fmt.Printf("asuka %s is the latest release\n", version)
			return nil
		}
		target = latest
	} else if target == version {
		fmt.Printf("asuka %s is this one\n", version)
		return nil
	}
	exe, err := os.Executable()
	if err != nil {
		return err
	}
	if exe, err = filepath.EvalSymlinks(exe); err != nil {
		return err
	}
	fmt.Fprintf(os.Stderr, "asuka: downloading %s\n", target)
	// The download runs here before it's put in place: it says its version.
	check := func(p string) error {
		cctx, cancel := context.WithTimeout(ctx, 30*time.Second)
		defer cancel()
		cmd := exec.CommandContext(cctx, p, "version")
		cmd.Env = append(os.Environ(), "ASUKA_NO_UPDATE_CHECK=1")
		out, err := cmd.Output()
		if err != nil {
			return err
		}
		if got := strings.TrimSpace(string(out)); got != "asuka "+target {
			return fmt.Errorf("it says it's %q", got)
		}
		return nil
	}
	if err := selfupdate.Upgrade(ctx, exe, target, check); err != nil {
		return err
	}
	fmt.Printf("asuka: %s is now %s (was %s)\n", exe, target, version)
	// The new release's libraries for this computer, as the install
	// scripts fetch them: its builds would otherwise wait for them first.
	// A failure isn't the upgrade's: a build fetches them when it needs one.
	fetch := exec.CommandContext(ctx, exe, "prebuilt", "fetch")
	fetch.Env = append(os.Environ(), "ASUKA_NO_UPDATE_CHECK=1")
	fetch.Stdout, fetch.Stderr = os.Stdout, os.Stderr
	if err := fetch.Run(); err != nil {
		fmt.Fprintln(os.Stderr, "asuka: the prebuilt libraries weren't fetched; the first build fetches them")
	}
	return nil
}

// updateCheckFile is where the check keeps what it found.
func updateCheckFile() string { return cachedir.Path("update-check.json") }

// startUpdateCheck starts the day's check for a newer release than this
// CLI, when it's on (selfupdate.Enabled) and due, in a process of its own
// (asuka _update-check) that this one never waits for; what it finds is
// said at the end of this command if it's in by then, else of a later
// one. Not for the commands whose output is another program's or an
// editor's (run, lsp), nor asuka upgrade's and the release's own.
func startUpdateCheck(cmd string) bool {
	if runtime.GOOS == "windows" {
		if exe, err := os.Executable(); err == nil {
			selfupdate.RemoveOld(exe) // what the last upgrade moved aside
		}
	}
	switch cmd {
	case "run", "lsp", "upgrade", "prebuilt", "_update-check":
		return false
	}
	if !selfupdate.Enabled(version, os.Getenv, isTerminal(os.Stderr)) {
		return false
	}
	if selfupdate.Due(updateCheckFile(), time.Now()) {
		if exe, err := os.Executable(); err == nil {
			c := exec.Command(exe, "_update-check")
			c.Env = append(os.Environ(), "ASUKA_NO_UPDATE_CHECK=1")
			detach(c) // stdin, stdout, stderr: the null device
			if c.Start() == nil {
				go c.Wait() // reaped if this run outlasts it
			}
		}
	}
	return true
}

// sayUpdate says, on stderr, what the last check found, if there's news.
func sayUpdate(on bool) {
	if !on {
		return
	}
	if n := selfupdate.Notice(updateCheckFile(), version, time.Now()); n != "" {
		fmt.Fprintln(os.Stderr, n)
	}
}
