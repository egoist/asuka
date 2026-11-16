package desktop_test

import (
	"bufio"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/lang/target"
)

// platform/location and platform/maps on Linux, their C on the test host (as
// TestStdLinux runs the other packages'): location against a fake
// location portal on the session bus (test/fakeportal.c: it allows or
// denies, then moves north), and with no location service at all; the
// map made and sized, with libshumate where it's installed (else the
// error the view raises). What can't run here: GeoClue (no system bus
// in the container), geocode-glib's answers (Nominatim, on the network),
// a user's clicks and drags on the map, its tiles (OpenStreetMap's).
func TestStdLinuxPlaces(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("the standard library's Linux C (run in the asuka-linux-test image)")
	}
	exe := stdHost(t)
	portal := placesPortal(t)
	_, geocoder := placesLib("libgeocode-glib-2.so.0")
	named := "host: named location: no geocoder (geocode-glib isn't installed)"
	if geocoder {
		named = "host: named"
	}
	t.Run("location", func(t *testing.T) {
		bus := placesBus(t)
		defer placesServe(t, portal, "allow", bus)()
		got := runPlaces(t, exe, `import platform/location

component Probe {
  state step = "start"
  Text("#{step}")
  Text("#auth {location.authorization().level == .always} {location.authorization().precise} {location.authorization().off}")
  task {
    do {
      step = "asked {location.authorization().level == .notAsked}"
      await sleep(1ms)
      let p = try await location.current()
      step = "current {p.latitude == 52.52} {p.longitude == 13.405} {p.accuracy == 10} {p.altitude == 34} {p.speed == 1.5} {p.course == 90} {p.time.unixNano() == 1700000000500000000}"
      await sleep(1ms)
      var n = 0
      var last = 0.0
      var north = true
      for await q in location.positions() {
        north = north && q.latitude > last
        last = q.latitude
        n += 1
        if n == 3 { break }
      }
      step = "watched {n} {north}"
      await sleep(1ms)
      let s = location.supported()
      step = "support {s.regions} {s.dwell} {s.updates}"
      await sleep(1ms)
      try await location.monitor(location.Region(id: "office", latitude: 52.523, longitude: 13.405, radius: 150))
      step = "monitored {(try await location.monitored()).count()}"
      await sleep(1ms)
      var crossed = 0
      for await c in location.crossings() {
        step = "crossed {c.region} {c.kind == .enter} {c.kind == .exit}"
        await sleep(1ms)
        crossed += 1
        if crossed == 2 { break }
      }
      try await location.stopMonitoringAll()
      try await location.startUpdates(distance: 10)
      step = "updating {try await location.updating()}"
      await sleep(1ms)
      for await b in location.updates() {
        step = "batch {b.positions.count()} {b.positions[0].latitude >= 52.52}"
        await sleep(1ms)
        break
      }
      location.stopUpdates()
      step = "stopped {try await location.updating()}"
      await sleep(1ms)
      do {
        let name = try await location.placeName(latitude: 52.52, longitude: 13.405)
        step = "named {name.count() > 0}"
      } catch e {
        step = "named {e.message}"
      }
      await sleep(1ms)
      step = "done"
    } catch e {
      step = "fail {e.message}"
    }
  }
}

app { Probe() }
`, "DBUS_SESSION_BUS_ADDRESS="+bus)
		placesWant(t, got, "host: asked true", "host: current true true true true true true true", "host: auth true true false",
			"host: watched 3 true", "host: support false true false", "host: monitored 1", "host: crossed office true false",
			"host: crossed office false true", "host: updating true", "host: batch 1 true", "host: stopped false", named, "host: done")
	})
	t.Run("location-denied", func(t *testing.T) {
		bus := placesBus(t)
		defer placesServe(t, portal, "deny", bus)()
		got := runPlaces(t, exe, `import platform/location

component Probe {
  state step = "start"
  Text("#{step}")
  Text("#level {location.authorization().level == .denied}")
  task {
    do {
      let _ = try await location.current()
      step = "fail: a position"
    } catch e {
      step = "current {e.message}"
    }
    await sleep(1ms)
    do {
      try await location.startUpdates()
      step = "fail: updates"
    } catch e {
      step = "updates {e.message}"
    }
    await sleep(1ms)
    step = "done"
  }
}

app { Probe() }
`, "DBUS_SESSION_BUS_ADDRESS="+bus)
		placesWant(t, got, "host: current location: not allowed", "host: level true", "host: updates location: not allowed", "host: done")
	})
	none := `import platform/location

component Probe {
  state step = "start"
  Text("#{step}")
  Text("#level {location.authorization().level == .notAsked} {location.authorization().level == .denied}")
  task {
    do {
      let _ = try await location.current()
      step = "fail: a position"
    } catch e {
      step = "current {e.message}"
    }
    await sleep(1ms)
    step = "done"
  }
}

app { Probe() }
`
	// No portal on the session bus (a bus of its own, which starts no
	// services) and no system bus: GeoClue isn't there either.
	t.Run("location-none", func(t *testing.T) {
		got := runPlaces(t, exe, none, "DBUS_SESSION_BUS_ADDRESS="+placesBus(t), "DBUS_SYSTEM_BUS_ADDRESS=unix:path=/nonexistent")
		placesWant(t, got, "host: current location: no location service (", "host: level true false", "host: done")
	})
	// The image's own xdg-desktop-portal, which has no GeoClue to ask.
	t.Run("location-portal", func(t *testing.T) {
		got := runPlaces(t, exe, none)
		placesWant(t, got, "host: current location: not allowed", "host: level false true", "host: done")
	})
	t.Run("maps", func(t *testing.T) {
		got := runPlaces(t, exe, `import platform/maps

component Probe {
  state step = "start"
  state region = maps.Region(center: maps.Coordinate(lat: 35.68, lng: 139.76), latDelta: 0.05, lngDelta: 0.05)
  Column {
    grow: 1
    Text("#{step}")
    maps.View {
      bind region: region
      markers: [
        maps.Marker(id: "tower", at: maps.Coordinate(lat: 35.6586, lng: 139.7454), title: "Tokyo Tower", subtitle: "Minato"),
        maps.Marker(id: "park", at: maps.Coordinate(lat: 35.6717, lng: 139.6949), title: "Yoyogi Park", color: #2e7d32),
      ]
      mapType: .satellite
      showsUser: true
      interactive: false
      grow: 1
      on error(m) { step = "error {m}" }
    }
  }
  task {
    await sleep(200ms)
    region = maps.Region(center: maps.Coordinate(lat: 48.8584, lng: 2.2945), latDelta: 0.02, lngDelta: 0.02)
    await sleep(100ms)
    step = "done"
  }
}

app { Probe() }
`)
		placesWant(t, got, "host: native view platform/maps#View", "host: measured platform/maps#View 390x", "host: done")
		if strings.Contains(got, "CRITICAL") {
			t.Errorf("GTK complained:\n%s", got)
		}
		if _, ok := placesLib("libshumate-1.0.so.1"); ok {
			if strings.Contains(got, "host: error") {
				t.Errorf("an error with libshumate:\n%s", got)
			}
		} else {
			placesWant(t, got, "host: error maps need libshumate (libshumate-1.0-1)")
		}
	})
}

// runPlaces runs an Asuka program on the test host, the lines it prints.
func runPlaces(t *testing.T, exe, src string, env ...string) string {
	t.Helper()
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "app.asuka"), []byte(src), 0o644)
	res, err := compile.Dir(dir, &compile.Options{Platform: target.Host()})
	if err != nil {
		t.Fatal(err)
	}
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	mod := filepath.Join(dir, "app.akb")
	os.WriteFile(mod, res.Module.Encode(), 0o644)
	cmd := exec.Command(exe, mod)
	cmd.Env = append(append(os.Environ(), "ASUKA_DATA="+dir, noLeaks, "ASUKA_TEST_WINDOW=1"), env...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
	return string(out)
}

func placesWant(t *testing.T, got string, lines ...string) {
	t.Helper()
	for _, w := range lines {
		if !strings.Contains(got, w) {
			t.Errorf("no %q in:\n%s", w, got)
		}
	}
}

// placesLib finds a shared library in the system's directories.
func placesLib(name string) (string, bool) {
	for _, d := range []string{"/usr/lib", "/usr/lib64", "/lib"} {
		if m, _ := filepath.Glob(filepath.Join(d, "*", name)); len(m) > 0 {
			return m[0], true
		}
		if _, err := os.Stat(filepath.Join(d, name)); err == nil {
			return filepath.Join(d, name), true
		}
	}
	return "", false
}

// placesPortal builds test/fakeportal.c.
func placesPortal(t *testing.T) string {
	cc, err := cbuild.Find()
	if err != nil {
		t.Fatal(err)
	}
	work := t.TempDir()
	src, _ := filepath.Abs(filepath.Join("test", "fakeportal.c"))
	objs, err := cc.CompileWith([]string{src}, work, []string{"-std=c17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-D_GNU_SOURCE"})
	if err != nil {
		t.Fatal(err)
	}
	exe := filepath.Join(work, "fakeportal")
	if err := cc.Link(objs, exe, []string{"-ldl"}); err != nil {
		t.Fatal(err)
	}
	return exe
}

// placesServe runs the fake portal (mode: allow, deny) on bus (a
// session bus of the test's own, so that no portal a test before
// started holds its name) until the returned stop.
func placesServe(t *testing.T, exe, mode, bus string) func() {
	cmd := exec.Command(exe, mode)
	cmd.Env = append(os.Environ(), "DBUS_SESSION_BUS_ADDRESS="+bus)
	cmd.Stderr = os.Stderr
	out, _ := cmd.StdoutPipe()
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	if line, _ := bufio.NewReader(out).ReadString('\n'); line != "ready\n" {
		cmd.Process.Kill()
		cmd.Wait()
		t.Fatalf("the fake portal didn't start: %q", line)
	}
	return func() {
		cmd.Process.Kill()
		cmd.Wait()
	}
}

// placesBus starts a D-Bus daemon of the test's own, which activates no
// services: its address.
func placesBus(t *testing.T) string {
	dir := t.TempDir()
	conf := filepath.Join(dir, "bus.conf")
	os.WriteFile(conf, []byte(`<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:dir=`+dir+`</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
`), 0o644)
	cmd := exec.Command("dbus-daemon", "--config-file="+conf, "--nofork", "--print-address=1")
	out, _ := cmd.StdoutPipe()
	if err := cmd.Start(); err != nil {
		t.Skipf("no dbus-daemon: %v", err)
	}
	t.Cleanup(func() {
		cmd.Process.Kill()
		cmd.Wait()
	})
	addr, err := bufio.NewReader(out).ReadString('\n')
	if err != nil {
		t.Fatal(err)
	}
	return strings.TrimSpace(addr)
}
