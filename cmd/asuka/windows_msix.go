package main

import (
	"bytes"
	"fmt"
	"image"
	"image/draw"
	"image/png"
	"os"
	"sort"
	"strconv"
	"strings"

	"github.com/egoist/asuka/internal/distpkg"
	iupdate "github.com/egoist/asuka/internal/update"
)

// A Windows app's MSIX (<name>-<version>-windows-<arch>.msix): the app's
// folder as a package, for the Microsoft Store (which takes it unsigned
// and signs it) and for sideloading (signed with ASUKA_WINDOWS_CERT, whose
// subject is the package's publisher: asuka.toml's windows.publisherId).
// Its manifest declares the app (a full-trust desktop app), its tiles'
// images (made from the icon), its URL scheme, its file types and its
// toasts' activator, which a packaged app can't register as it runs.
// Windows installs it, updates it (the Store, App Installer) and removes
// it: the app doesn't update itself (update.unsupported() is .managed).

// msixArch is the manifest's ProcessorArchitecture of a GOARCH.
func msixArch(goarch string) string {
	if goarch == "arm64" {
		return "arm64"
	}
	return "x64"
}

// msixIdentityName is the package's name: asuka.toml's
// windows.identityName, else the app's ID, as MSIX takes it (letters,
// digits, dots and dashes, 3 to 50 of them).
func msixIdentityName(app *asukaApp) string {
	if n := strings.TrimSpace(app.cfg.Windows.IdentityName); n != "" {
		return n
	}
	n := strings.Map(func(r rune) rune {
		if r < 0x80 && (r == '.' || r == '-' || r >= '0' && r <= '9' || r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z') {
			return r
		}
		return -1
	}, windowsAUMID(app))
	n = strings.Trim(n, ".")
	for len(n) < 3 {
		n += "App"
	}
	if len(n) > 50 {
		n = strings.Trim(n[len(n)-50:], ".")
	}
	return n
}

// msixPublisher is the package's publisher, a distinguished name:
// windows.publisherId, else CN= and the publisher's name.
func msixPublisher(app *asukaApp) string {
	if p := strings.TrimSpace(app.cfg.Windows.PublisherID); p != "" {
		return p
	}
	name := windowsPublisher(app)
	if strings.ContainsAny(name, `,=+<>#;"\`) {
		name = `"` + strings.ReplaceAll(name, `"`, `\"`) + `"`
	}
	return "CN=" + name
}

// msixVersion is the package's version: the app's major, minor and patch,
// and 0 (the Store's: it keeps the fourth part for itself).
func msixVersion(app *asukaApp) string {
	v := windowsVersion(app)
	return fmt.Sprintf("%d.%d.%d.0", v[0], v[1], v[2])
}

// An MSIX's images (the tiles', the taskbar's, Start's, the Store's)
// are made from the icon, each at every scale Windows draws them at
// (Assets\Square150x150Logo.scale-200.png), and the app's icon at the
// sizes the shell asks for (Square44x44Logo.targetsize-24.png), plated
// (on a tile's color, in Start's list) and unplated (as it is, on the
// taskbar, in the title bar, in Explorer: altform-unplated): its
// resources.pri (a package resource index, distpkg.WritePRI) lists them
// as the candidates of Assets\Square44x44Logo.png and the other names
// the manifest gives, and Windows picks the one for the display. The
// PRI also has the app's name and description in each of its catalogs'
// languages (asuka.appName, asuka.appDescription), which the manifest
// names as ms-resource:AppName: Start, the taskbar, Installed apps and
// the Store say them in the user's language.

// msixScales are the scales Windows has images at: 100% to 400%.
var msixScales = []int{100, 125, 150, 200, 400}

// msixTargetSizes are the sizes the shell asks the app's icon at.
var msixTargetSizes = []int{16, 24, 32, 48, 256}

// msixLogos are the manifest's images, by name, at 100%.
var msixLogos = []struct {
	name string
	w, h int
}{
	{"Square150x150Logo", 150, 150},
	{"Square44x44Logo", 44, 44},
	{"StoreLogo", 50, 50},
	{"Wide310x150Logo", 310, 150},
}

// An msixAsset is one of the images: its file in the package, its size,
// and its qualifiers in the PRI.
type msixAsset struct {
	logo, file string
	w, h       int
	qualifiers []distpkg.PRIQualifier
}

// msixAssets are the images, by their files' names, as makepri indexes
// them.
func msixAssets() []msixAsset {
	var out []msixAsset
	for _, l := range msixLogos {
		for _, s := range msixScales {
			out = append(out, msixAsset{l.name, fmt.Sprintf("%s.scale-%d.png", l.name, s), (l.w*s + 50) / 100, (l.h*s + 50) / 100,
				[]distpkg.PRIQualifier{{Type: distpkg.PRIScale, Value: strconv.Itoa(s)}}})
		}
		if l.name != "Square44x44Logo" {
			continue
		}
		for _, t := range msixTargetSizes {
			ts := distpkg.PRIQualifier{Type: distpkg.PRITargetSize, Value: strconv.Itoa(t)}
			out = append(out, msixAsset{l.name, fmt.Sprintf("%s.targetsize-%d.png", l.name, t), t, t, []distpkg.PRIQualifier{ts}},
				msixAsset{l.name, fmt.Sprintf("%s.targetsize-%d_altform-unplated.png", l.name, t), t, t,
					[]distpkg.PRIQualifier{ts, {Type: distpkg.PRIAlternateForm, Value: "unplated"}}})
		}
	}
	sort.Slice(out, func(i, j int) bool { return strings.ToUpper(out[i].file) < strings.ToUpper(out[j].file) })
	return out
}

// msixImage draws an asset from the icon at a size (nil: no icon, the
// image transparent): a square's the icon, a wide tile's the icon at its
// height, in the middle.
func msixImage(icon func(side int) *image.NRGBA, a msixAsset) []byte {
	img := image.NewNRGBA(image.Rect(0, 0, a.w, a.h))
	if icon != nil {
		side := min(a.w, a.h)
		at := image.Pt((a.w-side)/2, (a.h-side)/2)
		draw.Draw(img, image.Rectangle{at, at.Add(image.Pt(side, side))}, icon(side), image.Point{}, draw.Src)
	}
	var b bytes.Buffer
	png.Encode(&b, img)
	return b.Bytes()
}

// msixStrings are the manifest's words in each of the catalogs'
// languages, the source's first, a language that doesn't say one saying
// the source's (and the source asuka.toml's): the app's name and its
// description. They're nil without catalogs.
func msixStrings(app *asukaApp) (langs []string, byLang map[string][2]string) {
	all, source, cat := app.cfg.catalogStrings()
	if len(all) == 0 {
		return nil, nil
	}
	name, desc := app.displayName(), app.displayName()
	if d := strings.TrimSpace(app.cfg.Linux.Summary); d != "" {
		desc = d
	}
	pick := func(lang, key, def string) string {
		if v := strings.TrimSpace(cat[lang][key]); v != "" {
			return v
		}
		if v := strings.TrimSpace(cat[source][key]); v != "" {
			return v
		}
		return def
	}
	byLang = map[string][2]string{}
	langs = []string{source}
	for _, l := range all {
		if l != source {
			langs = append(langs, l)
		}
	}
	for _, l := range langs {
		byLang[l] = [2]string{pick(l, "appName", name), pick(l, "appDescription", desc)}
	}
	return langs, byLang
}

// msixPRI is the package's resources.pri: its images' candidates, and its
// words' in each language, as makepri indexes a folder of Assets and
// Strings\<language>\Resources.resw.
func msixPRI(app *asukaApp) ([]byte, error) {
	p := &distpkg.PRI{Name: msixIdentityName(app), DefaultLanguage: "en-us"}
	byLogo := map[string]int{}
	for _, a := range msixAssets() {
		key := "Files/Assets/" + a.logo + ".png"
		i, ok := byLogo[key]
		if !ok {
			i = len(p.Resources)
			byLogo[key] = i
			p.Resources = append(p.Resources, distpkg.PRIResource{Name: key})
		}
		p.Resources[i].Candidates = append(p.Resources[i].Candidates, distpkg.PRICandidate{Qualifiers: a.qualifiers, Value: `Assets\` + a.file, Path: true})
	}
	langs, words := msixStrings(app)
	if len(langs) > 0 {
		p.DefaultLanguage = langs[0]
		sorted := append([]string(nil), langs...)
		sort.Slice(sorted, func(i, j int) bool { return strings.ToUpper(sorted[i]) < strings.ToUpper(sorted[j]) })
		for k, key := range []string{"AppName", "Description"} {
			r := distpkg.PRIResource{Name: "Resources/" + key}
			for _, l := range sorted {
				r.Candidates = append(r.Candidates, distpkg.PRICandidate{Qualifiers: []distpkg.PRIQualifier{{Type: distpkg.PRILanguage, Value: l}}, Value: words[l][k]})
			}
			p.Resources = append(p.Resources, r)
		}
	}
	return distpkg.WritePRI(p)
}

func msixManifest(app *asukaApp, exe, goarch string) string {
	name := msixXML(app.displayName())
	desc := name
	if d := strings.TrimSpace(app.cfg.Linux.Summary); d != "" {
		desc = msixXML(d)
	}
	// In the user's language, from the PRI, with catalogs.
	plain := name
	if langs, _ := msixStrings(app); len(langs) > 0 {
		name, desc = "ms-resource:AppName", "ms-resource:Description"
	}
	var ext strings.Builder
	if s := app.cfg.Scheme; s != "" {
		fmt.Fprintf(&ext, `
        <uap:Extension Category="windows.protocol">
          <uap:Protocol Name="%s">
            <uap:DisplayName>%s</uap:DisplayName>
          </uap:Protocol>
        </uap:Extension>`, msixXML(strings.ToLower(s)), name)
	}
	for _, t := range app.cfg.fileTypes() {
		fmt.Fprintf(&ext, `
        <uap:Extension Category="windows.fileTypeAssociation">
          <uap:FileTypeAssociation Name="%s">
            <uap:DisplayName>%s</uap:DisplayName>
            <uap:SupportedFileTypes>`, msixXML(msixAssocName(t.extensions[0])), msixXML(t.name))
		for _, e := range t.extensions {
			fmt.Fprintf(&ext, `
              <uap:FileType>.%s</uap:FileType>`, msixXML(strings.ToLower(strings.TrimPrefix(e, "."))))
		}
		ext.WriteString(`
            </uap:SupportedFileTypes>
          </uap:FileTypeAssociation>
        </uap:Extension>`)
	}
	// The toasts' activator (platform/notify), the one the app registers
	// when it isn't packaged: COM starts the app for a toast's click.
	clsid := strings.Trim(toastActivatorCLSID(windowsAUMID(app)), "{}")
	fmt.Fprintf(&ext, `
        <com:Extension Category="windows.comServer">
          <com:ComServer>
            <com:ExeServer Executable="%s" Arguments="-ToastActivated" DisplayName="%s">
              <com:Class Id="%s" DisplayName="%s"/>
            </com:ExeServer>
          </com:ComServer>
        </com:Extension>
        <desktop:Extension Category="windows.toastNotificationActivation">
          <desktop:ToastNotificationActivation ToastActivatorCLSID="%s"/>
        </desktop:Extension>`, msixXML(exe), plain, clsid, plain, clsid)
	return fmt.Sprintf(`<?xml version="1.0" encoding="utf-8"?>
<Package
  xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
  xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
  xmlns:desktop="http://schemas.microsoft.com/appx/manifest/desktop/windows10"
  xmlns:com="http://schemas.microsoft.com/appx/manifest/com/windows10"
  xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
  IgnorableNamespaces="uap desktop com rescap">
  <Identity Name="%s" Publisher="%s" Version="%s" ProcessorArchitecture="%s"/>
  <Properties>
    <DisplayName>%s</DisplayName>
    <PublisherDisplayName>%s</PublisherDisplayName>
    <Logo>Assets\StoreLogo.png</Logo>
  </Properties>
  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.26100.0"/>
  </Dependencies>
  <Resources>%s
  </Resources>
  <Applications>
    <Application Id="App" Executable="%s" EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements DisplayName="%s" Description="%s" BackgroundColor="transparent"
        Square150x150Logo="Assets\Square150x150Logo.png" Square44x44Logo="Assets\Square44x44Logo.png">
        <uap:DefaultTile Wide310x150Logo="Assets\Wide310x150Logo.png"/>
      </uap:VisualElements>
      <Extensions>%s
      </Extensions>
    </Application>
  </Applications>
  <Capabilities>
    <Capability Name="internetClient"/>
    <rescap:Capability Name="runFullTrust"/>
  </Capabilities>
</Package>
`, msixXML(msixIdentityName(app)), msixXML(msixPublisher(app)), msixVersion(app), msixArch(goarch),
		name, msixXML(windowsPublisher(app)), msixResources(app), msixXML(exe), name, desc, ext.String())
}

// msixResources are the manifest's languages: its catalogs', the source's
// first (the package's default), which the Store lists the app in and
// Windows offers it in, and in which the PRI has its name; English
// without catalogs.
func msixResources(app *asukaApp) string {
	langs, _ := msixStrings(app)
	if len(langs) == 0 {
		langs = []string{"en-us"}
	}
	var b strings.Builder
	for _, l := range langs {
		fmt.Fprintf(&b, "\n    <Resource Language=\"%s\"/>", msixXML(l))
	}
	return b.String()
}

// msixAssocName is a file type association's name: its extension, lower
// case, of what the manifest takes (letters, digits, . _ -).
func msixAssocName(ext string) string {
	n := strings.Map(func(r rune) rune {
		if r >= 'a' && r <= 'z' || r >= '0' && r <= '9' || r == '.' || r == '_' || r == '-' {
			return r
		}
		return -1
	}, strings.ToLower(strings.TrimPrefix(ext, ".")))
	if n == "" {
		n = "file"
	}
	return n
}

func msixXML(s string) string {
	return strings.NewReplacer("&", "&amp;", "<", "&lt;", ">", "&gt;", `"`, "&quot;", "'", "&apos;").Replace(s)
}

// writeMSIX writes the MSIX of the app's folder (dir, exe in it) for
// goarch beside it, signed if a certificate is set, and returns it.
func writeMSIX(app *asukaApp, o *buildOptions, dir, exe, goarch string) (string, error) {
	files, err := distpkg.FromDir(dir, "", func(rel string) bool { return rel == iupdate.FileList })
	if err != nil {
		return "", err
	}
	var out []distpkg.File
	for _, f := range files {
		if f.IsDir() || f.IsLink() {
			continue
		}
		f.Path = strings.TrimPrefix(f.Path, "/")
		out = append(out, f)
	}
	// The icon at each size, once (a size is several images').
	var icon func(int) *image.NRGBA
	if app.cfg.Icon != "" {
		src, err := loadIcon(app.cfg.Icon)
		if err != nil {
			return "", fmt.Errorf("icon: %w", err)
		}
		sized := map[int]*image.NRGBA{}
		icon = func(side int) *image.NRGBA {
			if sized[side] == nil {
				sized[side] = resize(src, side)
			}
			return sized[side]
		}
	}
	for _, a := range msixAssets() {
		out = append(out, distpkg.File{Path: "Assets/" + a.file, Data: msixImage(icon, a)})
	}
	pri, err := msixPRI(app)
	if err != nil {
		return "", err
	}
	out = append(out, distpkg.File{Path: "resources.pri", Data: pri})
	file := releaseFile(app, o, "windows", goarch, ".msix")
	os.Remove(file)
	if err := distpkg.WriteMSIX(&distpkg.MSIX{Manifest: []byte(msixManifest(app, exe, goarch)), Files: out}, file); err != nil {
		return "", err
	}
	if _, err := signWindows(file, app.displayName(), o.verbose); err != nil {
		os.Remove(file)
		return "", err
	}
	return file, nil
}
