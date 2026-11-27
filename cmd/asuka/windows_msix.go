package main

import (
	"bytes"
	"fmt"
	"image/png"
	"os"
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

// msixAssets are the tiles' and the Store's images, from the icon (each
// at 4x: Windows scales them down), by their paths in the package.
var msixAssets = []struct {
	path string
	size int
}{
	{"Assets/Square44x44Logo.png", 176},
	{"Assets/Square150x150Logo.png", 600},
	{"Assets/StoreLogo.png", 200},
}

func msixManifest(app *asukaApp, exe, goarch string) string {
	name := msixXML(app.displayName())
	desc := name
	if d := strings.TrimSpace(app.cfg.Linux.Summary); d != "" {
		desc = msixXML(d)
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
        </desktop:Extension>`, msixXML(exe), name, clsid, name, clsid)
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
  <Resources>
    <Resource Language="en-us"/>
  </Resources>
  <Applications>
    <Application Id="App" Executable="%s" EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements DisplayName="%s" Description="%s" BackgroundColor="transparent"
        Square150x150Logo="Assets\Square150x150Logo.png" Square44x44Logo="Assets\Square44x44Logo.png"/>
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
		name, msixXML(windowsPublisher(app)), msixXML(exe), name, desc, ext.String())
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
	if app.cfg.Icon != "" {
		img, err := loadIcon(app.cfg.Icon)
		if err != nil {
			return "", fmt.Errorf("icon: %w", err)
		}
		for _, a := range msixAssets {
			var b bytes.Buffer
			if err := png.Encode(&b, resize(img, a.size)); err != nil {
				return "", err
			}
			out = append(out, distpkg.File{Path: a.path, Data: b.Bytes()})
		}
	} else {
		for _, a := range msixAssets {
			out = append(out, distpkg.File{Path: a.path, Data: blankPNG(a.size)})
		}
	}
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

// blankPNG is a transparent square PNG: an app with no icon's tiles.
func blankPNG(size int) []byte {
	var b bytes.Buffer
	png.Encode(&b, resize(nil, size))
	return b.Bytes()
}
