package main

import (
	"errors"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"

	"github.com/egoist/asuka/internal/gpgsign"
	iupdate "github.com/egoist/asuka/internal/update"
)

// asuka publish --platform mac|windows|linux: the release build, its update
// files (writeUpdates) required, uploaded where asuka.toml's updates say:
// a GitHub release of the version (with the gh CLI), or an S3 bucket
// (s3.go). The manifests go last, so that no app reads one whose files
// aren't there yet.

func desktopPlatform(p string) bool {
	return p == platformMac || p == platformWindows || p == platformLinux
}

func publishDesktop(app *asukaApp, o *publishOptions) error {
	u, err := app.cfg.updates()
	if err != nil {
		return err
	}
	// A Linux app's apt and dnf repository, and the key that signs it.
	var repo repoStore
	var repoKey *gpgsign.Key
	if o.platform == platformLinux {
		if repo, err = linuxRepoStore(app, o.repo); err != nil {
			return err
		}
		if repo != nil {
			if repoKey, err = linuxSigningKey(app); err != nil {
				return err
			}
			if repoKey == nil {
				return errors.New("a repository is signed with the Linux signing key: set ASUKA_LINUX_GPG_KEY or linux.signingKey (asuka linux keygen makes one)")
			}
		}
	}
	if u == nil && repo == nil {
		if o.platform == platformLinux {
			return errors.New("publishing a desktop app needs asuka.toml's updates (asuka update keygen prints them), or on Linux a repository (--repo, linux.repository)")
		}
		return errors.New("publishing a desktop app needs asuka.toml's updates: asuka update keygen prints them")
	}
	// Where to, and the key, before building.
	var bucket *s3Client
	if u != nil {
		switch {
		case u.S3 != nil:
			if bucket, err = newS3Client(u.S3); err != nil {
				return err
			}
		case u.GitHub != "":
			if _, err := exec.LookPath(ghCommand); err != nil {
				return errors.New("publishing to GitHub needs its CLI, gh (https://cli.github.com), signed in (gh auth login, or GH_TOKEN)")
			}
		default:
			return fmt.Errorf("asuka publish uploads to updates.github or updates.s3; with updates.url alone, build (asuka build) and upload dist/updates to %s yourself, the update-*.json manifests last", u.URL)
		}
		key, err := u.signingKey(app.dir)
		if err != nil {
			return err
		}
		if key == nil {
			return errors.New("publishing needs the update signing key, or installed apps couldn't update to it: set ASUKA_UPDATE_PRIVATE_KEY or updates.privateKey")
		}
	}
	b := o.buildOptions
	b.publishing = u != nil
	if err := b.setVersion(app); err != nil {
		return err
	}
	if b.out == "" {
		b.out = filepath.Join(app.dir, "dist")
	}
	if err := os.MkdirAll(b.out, 0o755); err != nil {
		return err
	}
	var target, goos, arch string
	switch b.platform {
	case platformMac:
		if arch, err = macArch(b.arch); err != nil {
			return err
		}
		goos, target = "darwin", macTarget(arch)
		err = buildMac(app, &b)
	case platformWindows:
		if arch, err = windowsArch(b.arch); err != nil {
			return err
		}
		goos, target = "windows", "windows-"+arch
		err = buildWindows(app, &b)
	case platformLinux:
		if arch, err = linuxArch(b.arch); err != nil {
			return err
		}
		goos, target = "linux", "linux-"+arch
		err = buildLinux(app, &b)
	}
	if err != nil {
		return err
	}
	if repo != nil {
		var debs, rpms []string
		for _, f := range releaseAssets(app, &b, goos, arch) {
			switch filepath.Ext(f) {
			case ".deb":
				debs = append(debs, f)
			case ".rpm":
				rpms = append(rpms, f)
			}
		}
		if len(debs)+len(rpms) == 0 {
			return errors.New("the build made no .deb or .rpm for the repository (--package, linux.packages)")
		}
		if err := publishLinuxRepo(app, repo, repoKey, debs, rpms); err != nil {
			return err
		}
	}
	if u == nil {
		return nil
	}
	files, manifests, err := updateFiles(filepath.Join(b.out, "updates"), exeSlug(app.displayName(), app.bundleID), app.cfg.Version, target)
	if err != nil {
		return err
	}
	// The packages to hand out go with the update: the .zip and .dmg; the
	// .zip, installer and .msix; the .deb, .rpm and AppImage (Linux's
	// .tar.gz is the update's archive).
	// The AppImage is both, uploaded once.
	for _, f := range releaseAssets(app, &b, goos, arch) {
		if !slices.ContainsFunc(files, func(g string) bool { return filepath.Base(g) == filepath.Base(f) }) {
			files = append(files, f)
		}
	}
	if bucket != nil {
		return publishS3(bucket, u, files, manifests)
	}
	notes, _ := u.releaseNotes(app.dir, app.cfg.Version)
	return publishGitHub(u, app.cfg.Version, notes, files, manifests, o.draft)
}

// updateFiles returns the update files of version for target in dir, and
// apart its manifests.
func updateFiles(dir, slug, version, target string) (files, manifests []string, err error) {
	archive := filepath.Join(dir, slug+"-"+version+"-"+target+".tar.gz")
	if _, err := os.Stat(archive); err != nil {
		return nil, nil, fmt.Errorf("the build wrote no update: %w", err)
	}
	files = append(files, archive)
	deltas, _ := filepath.Glob(filepath.Join(dir, slug+"-*-to-"+version+"-"+target+".delta"))
	files = append(files, deltas...)
	// A Linux build's AppImage and its .zsync (GitHub's, named for the
	// version); the .zsync of updates.url, named for none, which
	// AppImageUpdate reads as apps read the manifest, goes with it.
	for _, n := range []string{slug + "-" + version + "-" + target + ".AppImage", slug + "-" + version + "-" + target + ".AppImage.zsync"} {
		if p := filepath.Join(dir, n); fileExists(p) {
			files = append(files, p)
		}
	}
	manifest := filepath.Join(dir, iupdate.ManifestName(target, iupdate.Channel(version)))
	if _, err := os.Stat(manifest); err != nil {
		return nil, nil, err
	}
	manifests = []string{manifest}
	if p := filepath.Join(dir, appImageZsyncName(slug, version, target)); fileExists(p) {
		manifests = append(manifests, p)
	}
	return files, manifests, nil
}

// appImageZsyncName is the name of the .zsync of updates.url that an
// AppImage of version follows: its channel's, named for no version.
func appImageZsyncName(slug, version, target string) string {
	if c := iupdate.Channel(version); c != "" {
		return slug + "-" + target + "-" + c + ".AppImage.zsync"
	}
	return slug + "-" + target + ".AppImage.zsync"
}

// ghCommand is the GitHub CLI (tests replace it).
var ghCommand = "gh"

// publishGitHub uploads the files to the release tagged with the
// version (TagPrefix + version), a draft made if there's none, the
// manifests last; then publishes it unless draft, as a prerelease for a
// prerelease (which only apps on its channel take), and not as the latest
// with a tag prefix of the app's own (the repository's latest is another's).
func publishGitHub(u *updatesConfig, version, notes string, files, manifests []string, draft bool) error {
	repo, tag := u.GitHub, u.TagPrefix+version
	gh := func(args ...string) error {
		out, err := exec.Command(ghCommand, args...).CombinedOutput()
		if err != nil {
			return fmt.Errorf("gh %s: %v\n%s", args[0]+" "+args[1], err, out)
		}
		return nil
	}
	if exec.Command(ghCommand, "release", "view", tag, "--repo", repo).Run() != nil {
		args := []string{"release", "create", tag, "--repo", repo, "--draft", "--title", tag, "--notes", notes}
		if u.tagged() {
			args = append(args, "--latest=false")
		}
		if err := gh(args...); err != nil {
			return err
		}
		fmt.Printf("asuka: made the draft release %s of %s\n", tag, repo)
	}
	for _, batch := range [][]string{files, manifests} {
		if err := gh(append([]string{"release", "upload", tag, "--repo", repo, "--clobber"}, batch...)...); err != nil {
			return err
		}
		fmt.Printf("asuka: uploaded %s\n", names(batch))
	}
	edit := []string{"release", "edit", tag, "--repo", repo, "--draft=false"}
	if iupdate.Channel(version) != "" {
		edit = append(edit, "--prerelease")
	}
	if u.tagged() {
		edit = append(edit, "--latest=false")
	}
	if draft {
		fmt.Printf("asuka: uploaded to the draft release %s of %s; publish it once every platform is there:\n  gh %s\n", tag, repo, strings.Join(edit, " "))
		return nil
	}
	if err := gh(edit...); err != nil {
		return err
	}
	fmt.Printf("asuka: published the release %s of %s: installed apps update to it\n", tag, repo)
	return nil
}

// publishS3 uploads the files to the bucket that updates.url serves, the
// manifests last, which a CDN is told not to keep (their names stay from
// version to version).
func publishS3(bucket *s3Client, u *updatesConfig, files, manifests []string) error {
	for _, f := range slices.Concat(files, manifests) {
		name := filepath.Base(f)
		header := http.Header{}
		header.Set("Content-Type", assetType(name))
		if slices.Contains(manifests, f) {
			header.Set("Cache-Control", "no-cache")
		}
		if err := bucket.put(bucket.key(name), f, header); err != nil {
			return err
		}
		fmt.Printf("asuka: uploaded %s\n", name)
	}
	fmt.Printf("asuka: uploaded to s3://%s/%s, which %s serves: installed apps update to it\n", bucket.bucket, bucket.prefix, u.URL)
	return nil
}

// assetType is a release file's media type, as browsers and package
// managers take a download.
func assetType(name string) string {
	switch filepath.Ext(name) {
	case ".deb":
		return "application/vnd.debian.binary-package"
	case ".rpm":
		return "application/x-rpm"
	case ".AppImage":
		return "application/vnd.appimage"
	case ".dmg":
		return "application/x-apple-diskimage"
	case ".zip":
		return "application/zip"
	case ".exe":
		return "application/vnd.microsoft.portable-executable"
	case ".msix":
		return "application/msix"
	case ".json":
		return "application/json"
	case ".zsync":
		return "application/x-zsync"
	}
	return "application/octet-stream"
}

func names(paths []string) string {
	var n []string
	for _, p := range paths {
		n = append(n, filepath.Base(p))
	}
	return strings.Join(n, ", ")
}

// publishOTA writes an over-the-air update of a phone's or an Apple TV's
// app's code and uploads it where asuka.toml's updates say: the module,
// then its manifest, which installed builds of the platform and its
// fingerprint read.
func publishOTA(app *asukaApp, o *publishOptions) error {
	u, err := app.cfg.updates()
	if err != nil {
		return err
	}
	if u == nil {
		return errors.New("publishing an update needs asuka.toml's updates: asuka update keygen prints them")
	}
	var bucket *s3Client
	switch {
	case u.S3 != nil:
		if bucket, err = newS3Client(u.S3); err != nil {
			return err
		}
	case u.GitHub != "":
		if _, err := exec.LookPath(ghCommand); err != nil {
			return errors.New("publishing to GitHub needs its CLI, gh (https://cli.github.com), signed in (gh auth login, or GH_TOKEN)")
		}
	default:
		return fmt.Errorf("asuka publish uploads to updates.github or updates.s3; with updates.url alone, build the update (asuka build --update) and upload dist/updates to %s yourself, the manifest last", u.URL)
	}
	b := o.buildOptions
	if err := b.setVersion(app); err != nil {
		return err
	}
	if b.out == "" {
		b.out = filepath.Join(app.dir, "dist")
		if b.platform == platformTVOS {
			b.out = filepath.Join(b.out, "tvos") // as asuka build's
		}
	}
	file, manifest, err := writeOTAUpdate(app, b.platform, b.out)
	if err != nil {
		return err
	}
	if bucket != nil {
		return publishS3(bucket, u, []string{file}, []string{manifest})
	}
	notes, _ := u.releaseNotes(app.dir, app.cfg.Version)
	return publishGitHub(u, app.cfg.Version, notes, []string{file}, []string{manifest}, o.draft)
}
