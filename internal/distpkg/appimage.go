package distpkg

import (
	"bytes"
	"crypto/sha256"
	"debug/elf"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"time"

	"github.com/egoist/asuka/internal/ccache"
)

// An AppImage is the AppImage runtime (type 2: a static ELF that mounts
// what follows it with squashfuse, or extracts it) with a SquashFS image
// of the AppDir after it: AppRun, the app's desktop file and icon at its
// root, the app's prefix in usr/.

// RuntimeRelease is the AppImage runtime's release asuka builds with
// (github.com/AppImage/type2-runtime), and its files' SHA-256 by GOARCH.
const RuntimeRelease = "20251108"

var runtimeSHA256 = map[string]string{
	"amd64": "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d",
	"arm64": "00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444",
}

// runtimeArch is the runtime's file's architecture for a GOARCH.
var runtimeArch = map[string]string{"amd64": "x86_64", "arm64": "aarch64"}

// AppImageArch is the AppImage's architecture's name, as AppImages are
// named (x86_64, aarch64).
func AppImageArch(goarch string) string { return runtimeArch[goarch] }

// RuntimeURL is where the runtime for goarch is downloaded from.
func RuntimeURL(goarch string) string {
	return "https://github.com/AppImage/type2-runtime/releases/download/" + RuntimeRelease + "/runtime-" + runtimeArch[goarch]
}

// Runtime is the AppImage runtime for goarch: ASUKA_APPIMAGE_RUNTIME's
// file (one of your own: an offline build's, a newer release's), else
// the pinned release's, downloaded once into the build cache and checked
// against its SHA-256.
func Runtime(goarch string) ([]byte, error) {
	if p := os.Getenv("ASUKA_APPIMAGE_RUNTIME"); p != "" {
		return os.ReadFile(p)
	}
	sum, ok := runtimeSHA256[goarch]
	if !ok {
		return nil, fmt.Errorf("distpkg: no AppImage runtime for %s (amd64, arm64)", goarch)
	}
	file := filepath.Join(ccache.Dir("downloads"), sum, "runtime-"+runtimeArch[goarch])
	if b, err := os.ReadFile(file); err == nil && hexSHA256(b) == sum {
		ccache.Touch(filepath.Dir(file))
		return b, nil
	}
	u := RuntimeURL(goarch)
	fmt.Fprintf(os.Stderr, "asuka: downloading the AppImage runtime %s (once): %s\n", RuntimeRelease, u)
	b, err := fetch(u)
	if err != nil {
		return nil, fmt.Errorf("the AppImage runtime: %w (offline, set ASUKA_APPIMAGE_RUNTIME to a runtime-%s downloaded from %s)", err, runtimeArch[goarch], u)
	}
	if got := hexSHA256(b); got != sum {
		return nil, fmt.Errorf("the AppImage runtime %s: its SHA-256 is %s, not %s", u, got, sum)
	}
	if err := ccache.WriteFile(file, b); err != nil {
		return nil, err
	}
	return b, nil
}

func hexSHA256(b []byte) string {
	s := sha256.Sum256(b)
	return hex.EncodeToString(s[:])
}

// fetch GETs u: the environment's proxy, redirects followed, tried three
// times while the server fails.
func fetch(u string) ([]byte, error) {
	var last error
	for attempt := range 3 {
		if attempt > 0 {
			time.Sleep(time.Duration(attempt) * 2 * time.Second)
		}
		req, err := http.NewRequest("GET", u, nil)
		if err != nil {
			return nil, err
		}
		req.Header.Set("User-Agent", "asuka-cli")
		resp, err := (&http.Client{Timeout: 5 * time.Minute}).Do(req)
		if err != nil {
			last = err
			continue
		}
		b, err := io.ReadAll(io.LimitReader(resp.Body, 64<<20))
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			last = fmt.Errorf("GET %s: %s", u, resp.Status)
			if resp.StatusCode < 500 && resp.StatusCode != http.StatusTooManyRequests {
				return nil, last
			}
			continue
		}
		if err != nil {
			last = err
			continue
		}
		return b, nil
	}
	return nil, last
}

// AppImage is what an AppImage holds.
type AppImage struct {
	// Runtime is the AppImage runtime's ELF (Runtime).
	Runtime []byte
	// Files are the AppDir's, at their paths in it (/AppRun,
	// /usr/bin/notes...).
	Files []File
	// UpdateInfo is the update information AppImageUpdate reads
	// ("gh-releases-zsync|owner|repo|latest|notes-*-x86_64.AppImage.zsync"),
	// in the runtime's .upd_info section; "" for none.
	UpdateInfo string
	// Time dates its files.
	Time time.Time
	// Signer, when there's one, signs it as appimagetool --sign does
	// (AppImageSigner).
	Signer AppImageSigner
}

// An AppImageSigner signs an AppImage: an OpenPGP key (gpgsign.Key).
//
// An AppImage is signed as appimagetool --sign signs one, and as
// AppImageUpdate (its validate, appimageupdatetool) checks it: the
// SHA-256 of the whole file with its runtime's .sha256_sig and .sig_key
// sections zeroed, in lower case hex, is what's signed (a detached,
// armored, text mode OpenPGP signature); the signature goes in
// .sha256_sig, the armored public key in .sig_key.
type AppImageSigner interface {
	// SignArmoredText is a detached armored signature of text, in text
	// mode.
	SignArmoredText(text []byte) ([]byte, error)
	// ArmoredPublic is the public key, armored.
	ArmoredPublic() ([]byte, error)
}

// WriteAppImage writes a as an executable AppImage at file.
func WriteAppImage(a *AppImage, file string) error {
	rt, err := withUpdateInfo(a.Runtime, a.UpdateInfo)
	if err != nil {
		return err
	}
	mtime := a.Time
	if mtime.IsZero() {
		mtime = Epoch()
	}
	var img bytes.Buffer
	if err := WriteSquashFS(&img, a.Files, mtime); err != nil {
		return err
	}
	b := append(append(make([]byte, 0, len(rt)+img.Len()), rt...), img.Bytes()...) // rt may be a.Runtime
	if a.Signer != nil {
		if err := signAppImage(b, a.Signer); err != nil {
			return err
		}
	}
	tmp := file + ".tmp"
	if err := os.WriteFile(tmp, b, 0o755); err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, file)
}

// withUpdateInfo is the runtime with info in its .upd_info section (which
// it reserves, zeroed, for it), or as it is for none. The runtime must
// end where its ELF does: the image goes there.
func withUpdateInfo(rt []byte, info string) ([]byte, error) {
	f, err := elf.NewFile(bytes.NewReader(rt))
	if err != nil {
		return nil, fmt.Errorf("the AppImage runtime isn't an ELF: %w", err)
	}
	defer f.Close()
	if len(rt) < 11 || string(rt[8:11]) != "AI\x02" {
		return nil, errors.New("the AppImage runtime isn't a type 2 runtime (no AI\\x02 in its header)")
	}
	end := elfEnd(rt, f)
	if end != int64(len(rt)) {
		return nil, fmt.Errorf("the AppImage runtime is %d bytes, its ELF %d: not one the image can follow", len(rt), end)
	}
	if info == "" {
		return rt, nil
	}
	s := f.Section(".upd_info")
	if s == nil {
		return nil, errors.New("the AppImage runtime has no .upd_info section")
	}
	if uint64(len(info)) >= s.Size {
		return nil, fmt.Errorf("the AppImage's update information is %d bytes; its runtime has room for %d", len(info), s.Size-1)
	}
	out := append([]byte(nil), rt...)
	copy(out[s.Offset:s.Offset+s.Size], make([]byte, s.Size))
	copy(out[s.Offset:], info)
	return out, nil
}

// elfEnd is where an ELF's section headers end: the runtime finds the
// image there.
func elfEnd(b []byte, f *elf.File) int64 {
	bo := f.ByteOrder
	if f.Class == elf.ELFCLASS64 && len(b) >= 64 {
		return int64(bo.Uint64(b[0x28:])) + int64(bo.Uint16(b[0x3a:]))*int64(bo.Uint16(b[0x3c:]))
	}
	if len(b) >= 52 {
		return int64(bo.Uint32(b[0x20:])) + int64(bo.Uint16(b[0x2e:]))*int64(bo.Uint16(b[0x30:]))
	}
	return -1
}

// The runtime's sections an AppImage's signature and its key go in.
const (
	appImageSigSection = ".sha256_sig"
	appImageKeySection = ".sig_key"
)

// span is a section's bytes in the file: [off, off+size).
type span struct{ off, size int64 }

// appImageSection is where the named section of the AppImage b's runtime
// is.
func appImageSection(b []byte, name string) (span, error) {
	f, err := elf.NewFile(bytes.NewReader(b))
	if err != nil {
		return span{}, fmt.Errorf("the AppImage runtime isn't an ELF: %w", err)
	}
	defer f.Close()
	s := f.Section(name)
	if s == nil || s.Type == elf.SHT_NOBITS || s.Size == 0 {
		return span{}, fmt.Errorf("the AppImage runtime has no %s section", name)
	}
	if s.Offset+s.Size > uint64(len(b)) {
		return span{}, fmt.Errorf("the AppImage runtime's %s section is past its end", name)
	}
	return span{int64(s.Offset), int64(s.Size)}, nil
}

// AppImageDigest is what an AppImage's signature signs: the SHA-256 of
// b, the AppImage, with its .sha256_sig and .sig_key sections as zeros,
// in lower case hex.
func AppImageDigest(b []byte) (string, error) {
	sig, err := appImageSection(b, appImageSigSection)
	if err != nil {
		return "", err
	}
	key, err := appImageSection(b, appImageKeySection)
	if err != nil {
		return "", err
	}
	spans := []span{sig, key}
	if key.off < sig.off {
		spans = []span{key, sig}
	}
	if spans[0].off+spans[0].size > spans[1].off {
		return "", errors.New("the AppImage runtime's signature sections overlap")
	}
	h := sha256.New()
	zeros := make([]byte, 4096)
	pos := int64(0)
	for _, s := range spans {
		h.Write(b[pos:s.off])
		for n := s.size; n > 0; n -= min(n, int64(len(zeros))) {
			h.Write(zeros[:min(n, int64(len(zeros)))])
		}
		pos = s.off + s.size
	}
	h.Write(b[pos:])
	return hex.EncodeToString(h.Sum(nil)), nil
}

// signAppImage signs the AppImage b in place: its digest's signature and
// the public key in their sections, each followed by a zero byte at
// least, where the tools stop reading.
func signAppImage(b []byte, signer AppImageSigner) error {
	sig, err := appImageSection(b, appImageSigSection)
	if err != nil {
		return err
	}
	key, err := appImageSection(b, appImageKeySection)
	if err != nil {
		return err
	}
	clear(b[sig.off : sig.off+sig.size])
	clear(b[key.off : key.off+key.size])
	digest, err := AppImageDigest(b)
	if err != nil {
		return err
	}
	s, err := signer.SignArmoredText([]byte(digest))
	if err != nil {
		return fmt.Errorf("signing the AppImage: %w", err)
	}
	pub, err := signer.ArmoredPublic()
	if err != nil {
		return err
	}
	if int64(len(s)) >= sig.size {
		return fmt.Errorf("the AppImage's signature is %d bytes; its runtime's %s has room for %d", len(s), appImageSigSection, sig.size-1)
	}
	if int64(len(pub)) >= key.size {
		return fmt.Errorf("the AppImage's public key is %d bytes; its runtime's %s has room for %d", len(pub), appImageKeySection, key.size-1)
	}
	copy(b[sig.off:], s)
	copy(b[key.off:], pub)
	return nil
}

// AppImageSignature is the signature and the public key in the AppImage
// b, each as AppImageUpdate reads it (up to its section's first zero
// byte: none when it isn't signed), and the digest they sign.
func AppImageSignature(b []byte) (digest string, sig, key []byte, err error) {
	read := func(name string) ([]byte, error) {
		s, err := appImageSection(b, name)
		if err != nil {
			return nil, err
		}
		v := b[s.off : s.off+s.size]
		if i := bytes.IndexByte(v, 0); i >= 0 {
			v = v[:i]
		}
		return v, nil
	}
	if sig, err = read(appImageSigSection); err != nil {
		return "", nil, nil, err
	}
	if key, err = read(appImageKeySection); err != nil {
		return "", nil, nil, err
	}
	digest, err = AppImageDigest(b)
	return digest, sig, key, err
}
