package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"time"

	"github.com/egoist/asuka/internal/appbuild"
	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/target"
)

// androidABIs are the ABIs a release build carries: phones (arm64), old
// phones (armv7) and emulators and Chromebooks (x86_64).
// clang is the NDK's target triple (its clang's --target, the API level
// added).
var androidABIs = []struct{ abi, goarch, goarm, clang string }{
	{"arm64-v8a", "arm64", "", "aarch64-linux-android"},
	{"armeabi-v7a", "arm", "7", "armv7a-linux-androideabi"},
	{"x86_64", "amd64", "", "x86_64-linux-android"},
}

// buildAndroid makes a release build: an .aab for Google Play, signed
// with the upload key, the core with the app compiled in for every ABI, the assets and
// icons packaged, and the Kotlin shrunk by R8. No .apk: Google Play makes
// each device's from the .aab, with only that device's ABI and density.
func buildAndroid(app *asukaApp, o *buildOptions) error {
	start := time.Now()
	cfg := app.cfg
	tc, err := newAndroidToolchain(o.verbose)
	if err != nil {
		return err
	}
	work, err := workDir(app, "android-release")
	if err != nil {
		return err
	}
	// The upload key: asuka.toml, or the environment (CI).
	keystore := firstOf(os.Getenv("ASUKA_KEYSTORE"), cfg.Android.Keystore)
	alias := firstOf(os.Getenv("ASUKA_KEY_ALIAS"), cfg.Android.KeyAlias)
	storePass := os.Getenv("ASUKA_KEYSTORE_PASSWORD")
	if keystore == "" || alias == "" || storePass == "" {
		return errors.New(`a release build is signed with your upload key; make one (once, and keep it safe):
  keytool -genkeypair -v -keystore upload.jks -alias upload -keyalg RSA -keysize 2048 -validity 10000
then name it in asuka.toml's [android] (keystore = "upload.jks", keyAlias = "upload")
and give its password in ASUKA_KEYSTORE_PASSWORD (and ASUKA_KEY_PASSWORD if the key's differs)`)
	}
	fmt.Printf("asuka: building %s %s (%d) for Android\n", cfg.Name, cfg.Version, cfg.Build)

	jni, err := buildAndroidLibs(app, tc, work)
	if err != nil {
		return err
	}

	res := filepath.Join(work, "res")
	os.RemoveAll(res)
	icon := "@android:drawable/sym_def_app_icon"
	if cfg.Icon != "" {
		if err := androidIcons(cfg, res); err != nil {
			return err
		}
		icon = "@mipmap/ic_launcher"
	} else {
		fmt.Println("asuka: no icon in asuka.toml; Google Play needs one (512×512 or larger PNG)")
	}
	if _, err := androidRes(cfg, res); err != nil {
		return err
	}
	manifest := filepath.Join(work, "AndroidManifest.xml")
	if err := os.WriteFile(manifest, []byte(cfg.androidManifest(false)), 0o644); err != nil {
		return err
	}

	// The host's sources and Gradle build are shared: one build at a time.
	unlock, err := lockShared(androidHostDir(work))
	if err != nil {
		return err
	}
	defer unlock()
	src, _, err := prepareAndroidHost(app, work)
	if err != nil {
		return err
	}
	if err := tc.androidPrebuilt(app, src); err != nil {
		return err
	}
	task := ":app:bundleRelease"
	if o.apk {
		task = ":app:assembleRelease"
	}
	args := []string{task,
		"-PasukaApplicationId=" + androidPackage(cfg.ID), "-PasukaAppName=" + cfg.androidLabel(),
		"-PasukaVersionCode=" + strconv.Itoa(cfg.Build), "-PasukaVersionName=" + cfg.Version + cfg.VersionSuffix,
		"-PasukaIcon=" + icon, "-PasukaJniLibs=" + jni, "-PasukaRes=" + res, "-PasukaManifest=" + manifest,
		"-PasukaKeystore=" + keystore, "-PasukaKeyAlias=" + alias, "-PasukaNdkVersion=" + filepath.Base(tc.ndk)}
	args = append(args, cfg.gradleProps()...)
	// Passwords go in the environment, not on a command line.
	secrets := []string{"ORG_GRADLE_PROJECT_asukaKeystorePassword=" + storePass}
	if kp := os.Getenv("ASUKA_KEY_PASSWORD"); kp != "" {
		secrets = append(secrets, "ORG_GRADLE_PROJECT_asukaKeyPassword="+kp)
	}
	if assets := filepath.Join(app.dir, "assets"); exists(assets) {
		args = append(args, "-PasukaAssets="+assets)
	}
	fmt.Println("asuka: building with Gradle")
	if err := tc.gradleEnv(src, secrets, args...); err != nil {
		return err
	}
	name := safeName(cfg.Name)
	built := filepath.Join(src, "app", "build", "outputs")
	outs := []struct {
		from, to string
		optional bool
	}{
		{filepath.Join(built, "bundle", "release", "app-release.aab"), filepath.Base(aabPath(cfg, o)), o.apk},
		{filepath.Join(built, "apk", "release", "app-release.apk"), name + ".apk", !o.apk},
		// For a crash service (the .aab has both for Google Play): the
		// core's and the host's native symbols, and R8's map of the
		// Kotlin it renamed.
		{filepath.Join(built, "native-debug-symbols", "release", "native-debug-symbols.zip"), name + "-native-debug-symbols.zip", true},
		{filepath.Join(built, "mapping", "release", "mapping.txt"), name + "-mapping.txt", true},
	}
	for _, f := range outs {
		data, err := os.ReadFile(f.from)
		if err != nil {
			if f.optional {
				continue
			}
			return err
		}
		to := filepath.Join(o.out, f.to)
		if err := os.WriteFile(to, data, 0o644); err != nil {
			return err
		}
		fmt.Println("asuka:", to)
	}
	if o.apk {
		fmt.Printf("asuka: built in %.0fs; adb install installs it\n", time.Since(start).Seconds())
		return nil
	}
	fmt.Printf("asuka: built in %.0fs; asuka publish --platform android builds and uploads it to Google Play\n", time.Since(start).Seconds())
	return nil
}

func firstOf(vs ...string) string {
	for _, v := range vs {
		if v != "" {
			return v
		}
	}
	return ""
}

// buildAndroidLibs builds the C core with the app compiled in as a shared
// library for each ABI a release carries, in work/jniLibs/<abi>/libasuka.so,
// with its plugins' libraries beside it (plugins.go).
func buildAndroidLibs(app *asukaApp, tc *androidToolchain, work string) (string, error) {
	cfg := app.cfg
	jni := filepath.Join(work, "jniLibs")
	os.RemoveAll(jni)
	m, err := app.compile(target.Android)
	if err != nil {
		return "", err
	}
	updates, err := app.otaUpdates("android", m)
	if err != nil {
		return "", err
	}
	for _, a := range androidABIs {
		out := filepath.Join(jni, a.abi, "libasuka.so")
		if err := appbuild.LibWithUpdates(cbuild.Target{OS: "android", Arch: a.goarch}, cbuild.Shared, m, updates, out); err != nil {
			return "", fmt.Errorf("%s: %v", a.abi, err)
		}
	}
	// What the app's sources use decides its manifest's share sheet and
	// push rewrite, and whether it has Video's player.
	usedFeatures(app, cfg)
	cfg.androidFeatures(app, false)
	return jni, nil
}
