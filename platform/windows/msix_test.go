package windows_test

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The MSIX asuka build makes (--package msix), signed with a certificate
// of the test's (made in Windows, trusted for sideloading in its
// TrustedPeople store) through ASUKA_WINDOWS_CERT, as a developer
// sideloads: Add-AppxPackage installs it, its app runs from WindowsApps
// and its URL scheme (the manifest's) opens it, Remove-AppxPackage takes
// it away. Off Windows the build signs with osslsigncode. Its name is its
// resources.pri's, in each language: SHLoadIndirectString resolves the
// manifest's ms-resource:AppName through the PRI as the shell does (Start,
// the taskbar, Installed apps), for the thread's UI languages, and a
// language the app isn't in gets the source's; its images, the icon at
// the shell's sizes (targetsize-24, unplated on the taskbar).
func TestMSIX(t *testing.T) {
	w := onWindows(t)
	zig(t)
	if w.vm != "" {
		if _, err := exec.LookPath("osslsigncode"); err != nil {
			t.Skip("signing an MSIX off Windows needs osslsigncode")
		}
	}
	local, guest := w.job(t)
	root, err := filepath.Abs(filepath.Join("..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	// The app: Counter, its publisher the certificate's subject.
	app := filepath.Join(local, "app")
	copyTree(t, filepath.Join(root, "examples", "counter"), app)
	toml, err := os.ReadFile(filepath.Join(app, "asuka.toml"))
	if err != nil {
		t.Fatal(err)
	}
	toml = []byte(strings.Replace(string(toml), `id = "dev.asuka.counter"`, `id = "dev.asuka.msixtest"`+"\n"+`scheme = "asukamsixtest"`, 1) +
		"\n[windows]\npublisher = \"Asuka MSIX Test\"\n")
	if err := os.WriteFile(filepath.Join(app, "asuka.toml"), toml, 0o644); err != nil {
		t.Fatal(err)
	}
	// In English, German, Traditional Chinese and Arabic: the manifest's
	// languages, the app's name in each, its description in two.
	os.MkdirAll(filepath.Join(app, "locales"), 0o755)
	for tag, cat := range map[string]string{
		"en":      `{"asuka.appName": "Counter", "asuka.appDescription": "Counts taps"}`,
		"de":      `{"asuka.appName": "Zähler", "asuka.appDescription": "Zählt Tipps"}`,
		"zh-Hant": `{"asuka.appName": "計數器"}`,
		"ar":      `{"asuka.appName": "العداد"}`,
	} {
		if err := os.WriteFile(filepath.Join(app, "locales", tag+".json"), []byte(cat), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	pfx := guest + `\test.pfx`
	thumb := w.ps(t, `$c = New-SelfSignedCertificate -Type Custom -Subject 'CN=Asuka MSIX Test' -KeyUsage DigitalSignature -CertStoreLocation 'Cert:\CurrentUser\My' -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
Export-PfxCertificate -Cert $c -FilePath '`+pfx+`' -Password (ConvertTo-SecureString -String 'asuka' -Force -AsPlainText) | Out-Null
$c.Thumbprint`)
	thumb = thumb[strings.LastIndex(thumb, "\n")+1:]
	defer w.ps(t, `Remove-Item 'Cert:\CurrentUser\My\`+thumb+`', 'Cert:\LocalMachine\TrustedPeople\`+thumb+`' -ErrorAction SilentlyContinue; "ok"`)
	if out := w.ps(t, `try { Export-Certificate -Cert 'Cert:\CurrentUser\My\`+thumb+`' -FilePath $env:TEMP\asuka-msix.cer | Out-Null; Import-Certificate -FilePath $env:TEMP\asuka-msix.cer -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' | Out-Null; 'trusted' } catch { "not: $_" }`); !strings.HasSuffix(out, "trusted") {
		t.Skipf("trusting the test's certificate needs an administrator: %s", out)
	}

	out := filepath.Join(local, "dist")
	c := exec.Command("go", "run", "./cmd/asuka", "build", "--platform", "windows", "--arch", w.arch, "--package", "msix", "-o", out, app)
	c.Dir = root
	c.Env = append(os.Environ(), "ASUKA_WINDOWS_CERT="+w.local(pfx), "ASUKA_WINDOWS_CERT_PASSWORD=asuka", "ASUKA_WINDOWS_TIMESTAMP=none")
	if b, err := c.CombinedOutput(); err != nil {
		t.Fatalf("asuka build: %v\n%s", err, b)
	}
	msix := guest + `\dist\windows-` + w.arch + `\counter-1.0.0-windows-` + w.arch + `.msix`
	if _, err := os.Stat(w.local(msix)); err != nil {
		t.Fatal(err)
	}

	// What the shell shows of the package, through its PRI: its name and
	// description in the user's languages, its icon's file.
	os.WriteFile(filepath.Join(local, "names.ps1"), []byte(`param([string]$package, [string]$lang)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class AsukaRes {
  [DllImport("shlwapi.dll", CharSet = CharSet.Unicode)]
  static extern int SHLoadIndirectString(string src, StringBuilder buf, int cch, IntPtr reserved);
  public static string Load(string src) {
    var b = new StringBuilder(1024);
    int hr = SHLoadIndirectString(src, b, b.Capacity, IntPtr.Zero);
    return hr == 0 ? b.ToString() : "error 0x" + hr.ToString("x8");
  }
}
'@
$res = "@{${package}?ms-resource://dev.asuka.msixtest/"
$line = "$lang $([AsukaRes]::Load($res + 'Resources/AppName}')) / $([AsukaRes]::Load($res + 'Resources/Description}'))"
$line += " | $(@(Get-StartApps | Where-Object { $_.AppID -like 'dev.asuka.msixtest_*' } | ForEach-Object { $_.Name }) -join ',')"
$line += " | $([AsukaRes]::Load($res + 'Files/Assets/Square44x44Logo.png}'))"
Add-Content -Encoding UTF8 names.txt $line
`), 0o644)

	// In the signed-in session: deploying a package over the agent's is
	// denied.
	script := filepath.Join(local, "msix.ps1")
	os.WriteFile(script, []byte(`$ErrorActionPreference = 'Stop'
try {
  Copy-Item '`+msix+`' "$env:TEMP\asuka-test.msix" -Force
  Add-AppxPackage -Path "$env:TEMP\asuka-test.msix"
  $p = Get-AppxPackage -Name 'dev.asuka.msixtest'
  "installed $($p.PackageFamilyName) $($p.Version) $($p.SignatureKind)"
  $m = (Get-AppxPackageManifest -Package $p.PackageFullName).Package
  "languages $(@($m.Resources.Resource | ForEach-Object { $_.Language }) -join ',')"
  "display name $($m.Properties.DisplayName)"
  # Each language as the user's only one, resolved in a process of its
  # own (MRT reads the user's languages as a process starts).
  $saved = Get-WinUserLanguageList
  try {
    foreach ($l in 'en-US', 'de-DE', 'zh-TW', 'ar-SA', 'ja-JP') {
      Set-WinUserLanguageList -LanguageList (New-WinUserLanguageList $l) -Force -WarningAction SilentlyContinue
      & powershell.exe -NoProfile -ExecutionPolicy Bypass -File names.ps1 $p.PackageFullName $l
    }
  } finally {
    Set-WinUserLanguageList -LanguageList $saved -Force -WarningAction SilentlyContinue
  }
  Start-Process 'asukamsixtest://hello'
  for ($i = 0; $i -lt 40; $i++) {
    $run = Get-Process | Where-Object { $_.Path -like '*\WindowsApps\dev.asuka.msixtest_*\Counter.exe' }
    if ($run) { break }
    Start-Sleep -Milliseconds 250
  }
  if ($run) { "running $($run[0].Path)"; $run | Stop-Process -Force; Start-Sleep 1 }
  Remove-AppxPackage -Package $p.PackageFullName
  "removed $(@(Get-AppxPackage -Name 'dev.asuka.msixtest').Count)"
} catch {
  "failed: $_"
}
`), 0o644)
	res, err := w.run(t, guest, "powershell.exe", nil, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", guest+`\msix.ps1`)
	if err != nil {
		t.Fatalf("%v\n%s", err, res)
	}
	t.Logf("%s", res)
	for _, want := range []string{"installed dev.asuka.msixtest_", " 1.0.0.0 Developer", "languages en,ar,de,zh-Hant", "display name ms-resource:AppName", `running C:\Program Files\WindowsApps\dev.asuka.msixtest_1.0.0.0_`, "removed 0"} {
		if !strings.Contains(res, want) {
			t.Errorf("no %q:\n%s", want, res)
		}
	}
	b, err := os.ReadFile(filepath.Join(local, "names.txt"))
	if err != nil {
		t.Fatal(err)
	}
	names := strings.ReplaceAll(strings.TrimPrefix(string(b), "\ufeff"), "\r\n", "\n")
	t.Logf("%s", names)
	for _, want := range []string{
		"en-US Counter / Counts taps | Counter | ", "de-DE Zähler / Zählt Tipps | Zähler | ", "zh-TW 計數器 / Counts taps | 計數器 | ", "ar-SA العداد / Counts taps | العداد | ",
		"ja-JP Counter / Counts taps | Counter | ", // a language the app isn't in: the source's
		`\Assets\Square44x44Logo.scale-100.png`,
	} {
		if !strings.Contains(names, want) {
			t.Errorf("no %q in the names the PRI gives:\n%s", want, names)
		}
	}
}
