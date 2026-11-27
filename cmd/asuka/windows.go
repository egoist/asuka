package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"slices"
	"sort"
	"strconv"
	"strings"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/appconfig"
	"github.com/egoist/asuka/internal/cbuild"
	iupdate "github.com/egoist/asuka/internal/update"
	"github.com/egoist/asuka/lang/target"
)

// Windows: the app is one executable, the Windows host in C
// (platform/windows: Win32, the common controls, GDI+, DWM) with the C
// core and the app compiled to C (lang/cgen) linked in, for amd64 or
// arm64: asuka dev builds for this PC's, asuka build for amd64 unless
// --arch arm64 (an amd64 build runs on Windows on Arm too, emulated).
// Zig builds it from any system; on Windows, clang or MinGW's gcc too
// (ASUKA_CC).
//
// asuka dev --platform windows runs the player here (the host and the
// core without an app, a console program), which loads the app's
// bytecode from asuka dev and again on each save, keeping its state
// (Windows can't send a process SIGINT, so Asuka closes its standard
// input to stop it, ASUKA_STOP=stdin). asuka build --platform windows
// makes a folder with the .exe, its icon, manifest and version
// information built in (a COFF object of resources linked into it), the
// app's settings (asuka-app.json) and its assets beside it. With
// ASUKA_WINDOWS_CERT (a .pfx) and ASUKA_WINDOWS_CERT_PASSWORD set, it
// signs the .exe, timestamped: with signtool on Windows, osslsigncode
// elsewhere.

// windowsArch is a Windows build's GOARCH: arch (amd64 or arm64), else
// amd64, which runs on every Windows PC.
func windowsArch(arch string) (string, error) {
	switch arch {
	case "":
		return "amd64", nil
	case "amd64", "arm64":
		return arch, nil
	}
	return "", fmt.Errorf("--arch %q: amd64 or arm64", arch)
}

func buildWindows(app *asukaApp, o *buildOptions) error {
	goarch, err := windowsArch(o.arch)
	if err != nil {
		return err
	}
	pkgs, err := packagesFor(o, app.cfg.Windows.Packages, windowsPackages, "windows")
	if err != nil {
		return err
	}
	name := app.displayName()
	exe := sanitizeExe(name) + ".exe"
	out := filepath.Join(desktopDir(o, "windows", goarch), sanitizeExe(name))
	if err := os.RemoveAll(out); err != nil {
		return err
	}
	if err := os.MkdirAll(out, 0o755); err != nil {
		return err
	}
	// The host, with the resources (an icon, the manifest, the version)
	// linked in, the core with the app compiled in beside it, and the
	// app's settings.
	res, err := winresSyso(app, goarch)
	if err != nil {
		return err
	}
	// An app's own resources (a .syso in its directory, for the
	// architecture: rsrc_windows_arm64.syso) instead.
	if own, _ := filepath.Glob(filepath.Join(app.dir, "*.syso")); len(own) > 0 {
		pick := own[0]
		for _, f := range own {
			if strings.Contains(filepath.Base(f), "_"+goarch) {
				pick = f
			}
		}
		if res, err = os.ReadFile(pick); err != nil {
			return err
		}
		fmt.Println("asuka: using the app's own resources,", filepath.Base(pick))
	}
	target := "windows-" + goarch
	t := cbuild.Target{OS: "windows", Arch: goarch}
	if err := windowsRelease(app, t, res, filepath.Join(out, exe), o.verbose); err != nil {
		return err
	}
	if err := writeHostConfig(app, platformWindows, target, true, filepath.Join(out, appconfig.File)); err != nil {
		return err
	}
	signed, err := signWindows(filepath.Join(out, exe), name, o.verbose)
	if err != nil {
		return err
	}
	// The project's programs beside the .exe, signed as it is.
	progs, err := bundlePrograms(app, t, out, o.verbose)
	if err != nil {
		return err
	}
	for _, p := range progs {
		if _, err := signWindows(p, name, o.verbose); err != nil {
			return err
		}
	}
	if err := copyDir(filepath.Join(app.dir, "assets"), filepath.Join(out, "assets")); err != nil {
		return err
	}
	fmt.Printf("asuka: %s (windows/%s)\n", filepath.Join(out, exe), goarch)
	if u, _ := app.cfg.updates(); u != nil {
		// The folder's file list, then its update: the folder's entries.
		b := updateBuild{target: target, kind: iupdate.Folder, dir: out, root: out}
		if err := writeFileList(b); err != nil {
			return err
		}
		entries, err := os.ReadDir(out)
		if err != nil {
			return err
		}
		for _, e := range entries {
			b.entries = append(b.entries, e.Name())
		}
		if _, err := writeUpdates(app, o.out, b, o.publishing); err != nil {
			return err
		}
	} else if o.publishing {
		_, err := writeUpdates(app, o.out, updateBuild{}, true)
		return err
	}
	// What a build before made that this one doesn't goes.
	removeUnmade(app, o, "windows", goarch, map[string]string{pkgZip: ".zip", pkgInstaller: "-setup.exe", pkgMSIX: ".msix"}, pkgs)
	if pkgs[pkgZip] {
		// The folder, zipped, named for its version and architecture.
		zip := releaseFile(app, o, "windows", goarch, ".zip")
		if err := zipDir(out, zip); err != nil {
			return err
		}
		fmt.Printf("asuka: %s\n", zip)
	}
	if pkgs[pkgInstaller] {
		setup, err := writeWindowsInstaller(app, o, out, exe, goarch)
		if err != nil {
			return fmt.Errorf("the installer: %w", err)
		}
		fmt.Printf("asuka: %s (installs for the user: %%LOCALAPPDATA%%\\Programs\\%s)\n", setup, sanitizeExe(name))
	}
	if pkgs[pkgMSIX] {
		msix, err := writeMSIX(app, o, out, exe, goarch)
		if err != nil {
			return fmt.Errorf("the MSIX: %w", err)
		}
		if signed {
			fmt.Printf("asuka: %s (%s, signed: its certificate's subject must be %s)\n", msix, msixIdentityName(app), msixPublisher(app))
		} else {
			fmt.Printf("asuka: %s (%s, %s; not signed: for the Microsoft Store, which signs it)\n", msix, msixIdentityName(app), msixPublisher(app))
		}
	}
	if !signed {
		fmt.Println("asuka: set ASUKA_WINDOWS_CERT and ASUKA_WINDOWS_CERT_PASSWORD to sign the .exe and the packages, and keep SmartScreen quiet")
	}
	return nil
}

// windowsRelease builds a release's executable for t at exe: the host,
// the core and the app compiled to C, with the C it calls (its externs',
// the std packages': std/sqlite's SQLite) and the resources (a COFF
// object) linked in.
func windowsRelease(app *asukaApp, t cbuild.Target, res []byte, exe string, verbose bool) error {
	m, err := app.compile(target.Windows)
	if err != nil {
		return err
	}
	c, err := appbuild.AppC(m)
	if err != nil {
		return err
	}
	tmp, err := os.MkdirTemp("", "asuka-res")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)
	obj := filepath.Join(tmp, "resources.o")
	if err := os.WriteFile(obj, res, 0o644); err != nil {
		return err
	}
	natives, err := appbuild.DesktopNatives(t, desktopNativeDirs(app))
	if err != nil {
		return err
	}
	if verbose {
		fmt.Printf("asuka: building the Windows host for %s/%s\n", t.OS, t.Arch)
	}
	return appbuild.WindowsHost(t, appbuild.WindowsOptions{App: []string{c}, Builtins: m.Builtins, C: m.C, Native: natives, Resources: obj}, exe)
}

// defaultTimestamps are the RFC 3161 timestamp servers a signature is
// timestamped by, so it stays valid after the certificate expires: the
// first that answers.
var defaultTimestamps = []string{
	"http://timestamp.digicert.com",
	"http://timestamp.sectigo.com",
	"http://timestamp.globalsign.com/tsa/r6advanced1",
}

// timestampServers are the servers ASUKA_WINDOWS_TIMESTAMP names (a
// comma-separated list; "none": no timestamp), else defaultTimestamps.
func timestampServers(env string) []string {
	env = strings.TrimSpace(env)
	if env == "" {
		return defaultTimestamps
	}
	if strings.EqualFold(env, "none") {
		return nil
	}
	var out []string
	for _, s := range strings.Split(env, ",") {
		if s = strings.TrimSpace(s); s != "" {
			out = append(out, s)
		}
	}
	return out
}

// signWindows signs exe with the certificate in ASUKA_WINDOWS_CERT (a
// .pfx, its password ASUKA_WINDOWS_CERT_PASSWORD), SHA-256 and timestamped
// (by ASUKA_WINDOWS_TIMESTAMP's servers, else defaultTimestamps', the
// first that answers; "none": not timestamped): with signtool (the
// Windows SDK's) on Windows, osslsigncode elsewhere. Without a
// certificate it leaves exe unsigned.
func signWindows(exe, name string, verbose bool) (bool, error) {
	if cmd := strings.TrimSpace(os.Getenv("ASUKA_WINDOWS_SIGN_COMMAND")); cmd != "" {
		return true, signWithCommand(cmd, exe, verbose)
	}
	cert := os.Getenv("ASUKA_WINDOWS_CERT")
	if cert == "" {
		return false, nil
	}
	if _, err := os.Stat(cert); err != nil {
		return false, fmt.Errorf("ASUKA_WINDOWS_CERT: %w", err)
	}
	password, ok := os.LookupEnv("ASUKA_WINDOWS_CERT_PASSWORD")
	if !ok {
		return false, errors.New("ASUKA_WINDOWS_CERT is set but not ASUKA_WINDOWS_CERT_PASSWORD (set it empty for a .pfx without one)")
	}
	servers := timestampServers(os.Getenv("ASUKA_WINDOWS_TIMESTAMP"))
	tries := servers
	if len(tries) == 0 {
		tries = []string{""} // not timestamped
	}
	var failed []string
	for _, ts := range tries {
		tool, args, err := windowsSigner(exe, name, cert, password, ts)
		if err != nil {
			return false, err
		}
		if verbose {
			fmt.Println("+", tool, strings.Join(redact(args, password), " "))
		}
		b, err := exec.Command(tool, args...).CombinedOutput()
		if err != nil {
			os.Remove(exe + ".signed")
			out := strings.ReplaceAll(string(b), password, "***")
			if ts != "" && timestampFailed(out) {
				// The server didn't answer: the next one.
				failed = append(failed, ts)
				if verbose {
					fmt.Printf("asuka: %s didn't timestamp the signature:\n%s\n", ts, out)
				}
				continue
			}
			return false, fmt.Errorf("signing %s failed: %v\n%s", filepath.Base(exe), err, out)
		}
		if !strings.EqualFold(filepath.Base(tool), "signtool.exe") {
			// osslsigncode writes a new file.
			if err := os.Rename(exe+".signed", exe); err != nil {
				return false, err
			}
		}
		if ts == "" {
			fmt.Printf("asuka: signed %s, not timestamped (ASUKA_WINDOWS_TIMESTAMP=none): its signature ends with the certificate\n", filepath.Base(exe))
		} else {
			fmt.Printf("asuka: signed %s (timestamped by %s)\n", filepath.Base(exe), ts)
		}
		return true, nil
	}
	return false, fmt.Errorf("signing %s: no timestamp server answered (%s): check the network, or set ASUKA_WINDOWS_TIMESTAMP to other RFC 3161 servers (comma-separated), or to none to sign without a timestamp (the signature then ends when the certificate does)",
		filepath.Base(exe), strings.Join(failed, ", "))
}

// signWithCommand signs file with ASUKA_WINDOWS_SIGN_COMMAND, a command
// of the user's (a hardware token's tool, Azure Trusted Signing's,
// AzureSignTool), %1 in it the file's quoted path, run by the system's
// shell (cmd on Windows, sh elsewhere): what electron-builder's custom
// sign and Tauri's signCommand do.
func signWithCommand(command, file string, verbose bool) error {
	if !strings.Contains(command, "%1") {
		return errors.New(`ASUKA_WINDOWS_SIGN_COMMAND has no %1, where the file to sign goes ("signtool sign /sha1 … /fd SHA256 %1")`)
	}
	var c *exec.Cmd
	if runtime.GOOS == "windows" {
		line := strings.ReplaceAll(command, "%1", `"`+file+`"`)
		c = exec.Command("cmd.exe")
		c.SysProcAttr = cmdLine("/d /s /c \"" + line + "\"")
	} else {
		c = exec.Command("sh", "-c", strings.ReplaceAll(command, "%1", `"$1"`), "sign", file)
	}
	if verbose {
		fmt.Println("+", strings.ReplaceAll(command, "%1", file))
	}
	if b, err := c.CombinedOutput(); err != nil {
		return fmt.Errorf("signing %s with ASUKA_WINDOWS_SIGN_COMMAND failed: %v\n%s", filepath.Base(file), err, b)
	}
	fmt.Printf("asuka: signed %s (ASUKA_WINDOWS_SIGN_COMMAND)\n", filepath.Base(file))
	return nil
}

// timestampFailed reports whether a signer's output says the timestamp
// server failed (osslsigncode: "RFC 3161 timestamping failed"; signtool:
// "...the specified timestamp server either could not be reached...").
func timestampFailed(out string) bool {
	o := strings.ToLower(out)
	return strings.Contains(o, "timestamping failed") || strings.Contains(o, "timestamp server") ||
		strings.Contains(o, "while attempting to timestamp")
}

// windowsSigner is the command that signs exe, timestamped by ts (if not
// ""): signtool on Windows (on the PATH, else the newest Windows SDK's),
// osslsigncode elsewhere.
func windowsSigner(exe, name, cert, password, ts string) (string, []string, error) {
	if runtime.GOOS == "windows" {
		tool, err := exec.LookPath("signtool.exe")
		if err != nil {
			tool = sdkSigntool()
		}
		if tool == "" {
			return "", nil, errors.New("ASUKA_WINDOWS_CERT is set, but signtool isn't found: install the Windows SDK, or put signtool.exe on the PATH")
		}
		args := []string{"sign", "/f", cert, "/p", password, "/fd", "SHA256"}
		if ts != "" {
			args = append(args, "/tr", ts, "/td", "SHA256")
		}
		return tool, append(args, "/d", name, exe), nil
	}
	tool, err := exec.LookPath("osslsigncode")
	if err != nil {
		return "", nil, errors.New("ASUKA_WINDOWS_CERT is set, but osslsigncode isn't found (it signs Windows executables off Windows: brew install osslsigncode, apt install osslsigncode)")
	}
	args := []string{"sign", "-pkcs12", cert, "-pass", password, "-h", "sha256", "-n", name}
	if ts != "" {
		args = append(args, "-ts", ts)
	}
	return tool, append(args, "-in", exe, "-out", exe+".signed"), nil
}

// sdkSigntool is the newest Windows SDK's signtool for this PC, "" for
// none.
func sdkSigntool() string {
	arch := "x64"
	if runtime.GOARCH == "arm64" {
		arch = "arm64"
	}
	base := filepath.Join(os.Getenv("ProgramFiles(x86)"), "Windows Kits", "10", "bin")
	m, _ := filepath.Glob(filepath.Join(base, "10.*", arch, "signtool.exe"))
	if len(m) == 0 {
		m, _ = filepath.Glob(filepath.Join(base, "10.*", "x64", "signtool.exe"))
	}
	if len(m) == 0 {
		return ""
	}
	sort.Slice(m, func(i, j int) bool { return sdkVersionLess(m[i], m[j]) })
	return m[len(m)-1]
}

// sdkVersionLess orders paths by the SDK version in them (10.0.22621.0).
func sdkVersionLess(a, b string) bool {
	va := strings.Split(filepath.Base(filepath.Dir(filepath.Dir(a))), ".")
	vb := strings.Split(filepath.Base(filepath.Dir(filepath.Dir(b))), ".")
	for i := 0; i < len(va) && i < len(vb); i++ {
		x, _ := strconv.Atoi(va[i])
		y, _ := strconv.Atoi(vb[i])
		if x != y {
			return x < y
		}
	}
	return len(va) < len(vb)
}

// redact is args with the password hidden, to print.
func redact(args []string, password string) []string {
	out := slices.Clone(args)
	for i, a := range out {
		if password != "" && a == password {
			out[i] = "***"
		}
	}
	return out
}
