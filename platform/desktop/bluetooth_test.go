package desktop_test

import (
	"bufio"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/cbuild"
	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/lang/target"
)

// platform/bluetooth's Linux C (BlueZ over D-Bus) against a BlueZ of the
// test's own (test/bluez.c): org.bluez on the session bus, which the app
// takes for the system's (DBUS_SYSTEM_BUS_ADDRESS), with an adapter, a
// heart rate monitor to scan, connect to, read, write and hear, and the
// calls BlueZ makes of the app's advertisement and GATT application.
// Run under dbus-run-session (the asuka-linux-test image).
func TestBluetoothLinux(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("BlueZ's (run in the asuka-linux-test image)")
	}
	session := os.Getenv("DBUS_SESSION_BUS_ADDRESS")
	if session == "" {
		t.Skip("no session bus (run under dbus-run-session)")
	}
	exe := stdHost(t)
	core, err := cbuild.CoreDir()
	if err != nil {
		t.Fatal(err)
	}
	desk, _ := filepath.Abs(".")
	work := t.TempDir()
	var srcs []string
	for _, f := range []string{"test/bluez.c", "test/unit.c", "src/native.c", "src/json.c", "src/linux.c", "src/linux_syms.c"} {
		srcs = append(srcs, filepath.Join(desk, f))
	}
	flags := append(append([]string{}, cbuild.Flags...), "-I"+filepath.Join(desk, "include"), "-D_GNU_SOURCE")
	fake, err := cbuild.Exe(core, srcs, flags, nil, true, work, "bluez")
	if err != nil {
		t.Fatal(err)
	}
	bluez := exec.Command(fake)
	bluez.Env = append(os.Environ(), noLeaks)
	bluez.Stderr = os.Stderr
	out, err := bluez.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := bluez.Start(); err != nil {
		t.Fatal(err)
	}
	lines := make(chan string, 256)
	go func() {
		s := bufio.NewScanner(out)
		for s.Scan() {
			lines <- s.Text()
		}
		close(lines)
	}()
	defer bluez.Process.Kill()
	select {
	case l := <-lines:
		if l != "bluez: ready" {
			t.Fatalf("the fake BlueZ said %q", l)
		}
	case <-time.After(30 * time.Second): // a safety net, not what passes
		t.Fatal("the fake BlueZ never started")
	}

	app := t.TempDir()
	os.WriteFile(filepath.Join(app, "app.asuka"), []byte(bluetoothProbe), 0o644)
	res, err := compile.Dir(app, &compile.Options{Platform: target.Host()})
	if err != nil {
		t.Fatal(err)
	}
	if err := res.Err(); err != nil {
		t.Fatal(err)
	}
	mod := filepath.Join(app, "app.akb")
	os.WriteFile(mod, res.Module.Encode(), 0o644)
	cmd := exec.Command(exe, mod)
	cmd.Env = append(os.Environ(), "ASUKA_DATA="+app, noLeaks, "DBUS_SYSTEM_BUS_ADDRESS="+session)
	got, err := cmd.CombinedOutput()
	bluez.Process.Kill()
	bluez.Wait()
	var said []string
	for l := range lines {
		said = append(said, l)
	}
	all := string(got) + strings.Join(said, "\n")
	t.Logf("%s", all)
	if err != nil {
		t.Fatalf("%v", err)
	}
	for _, w := range []string{
		"host: state on",
		"bluez: filter 0000180d-0000-1000-8000-00805f9b34fb",
		"bluez: StartDiscovery",
		"host: heard AA:BB:CC:DD:EE:FF Polar H10 -60 Polar H10 180d true true",
		"bluez: StopDiscovery",
		"bluez: Connect",
		"host: services 180d:2a37/3/2902",
		"host: read 01 02 03",
		"bluez: written 07 00 request",
		"host: mtu 185",
		"bluez: StartNotify",
		"host: notified 09 08",
		"host: lost bluetooth: Connection terminated by remote user",
		"host: subscription ended bluetooth: disconnected",
		"host: read after: bluetooth: disconnected",
		"host: off",
		"host: scan while off: bluetooth: off",
		"host: choose: bluetooth: not available",
		"bluez: advertisement asuka type=peripheral discoverable=1 uuids=1 180d company=76 data=01 02",
		"bluez: app characteristic 2a37 flags=read,notify",
		"bluez: app characteristic 2a38 flags=encrypt-write",
		"bluez: central read 05 06",
		"bluez: central wrote: ok",
		"bluez: subscribed",
		"host: subscribers 1",
		"bluez: notified 2a",
		"bluez: UnregisterApplication",
		"bluez: released the advertisement",
		"host: advertising threw",
		"host: done",
	} {
		if !strings.Contains(all, w) {
			t.Errorf("no %q", w)
		}
	}
}

const bluetoothProbe = `import platform/bluetooth

let level = bluetooth.LocalCharacteristic(uuid: "2a37", properties: [.read, .notify], permissions: [.readable], onRead: _ => bytes([4, 5, 6]))
let point = bluetooth.LocalCharacteristic(uuid: "2a38", properties: [.write], permissions: [.writeEncrypted])

component Probe {
  state step = "start"
  state serving = false
  state advertising = false
  state ended = ""
  Text("#{step}")

  task(serving) {
    if !serving {
      return
    }
    do {
      try await bluetooth.serve([bluetooth.LocalService(uuid: "180d", characteristics: [level, point])])
    } catch e {
      step = "serve failed {e.message}"
    }
  }

  task(advertising) {
    if !advertising {
      return
    }
    do {
      try await bluetooth.advertise(localName: "asuka", services: ["180d"], manufacturerData: ["004c": bytes([1, 2])])
      ended = "returned"
    } catch e {
      ended = "threw"
    }
  }

  task {
    do {
      // The peripheral: advertised and served; BlueZ's central reads,
      // writes and subscribes; the app notifies, then stops serving,
      // and BlueZ takes the advertisement back.
      advertising = true
      serving = true
      while level.subscribers().count() == 0 {
        await sleep(5ms)
      }
      step = "subscribers {level.subscribers().count()}"
      await sleep(1ms)
      level.notify(bytes([42]))
      serving = false
      while ended.isEmpty() {
        await sleep(5ms)
      }
      step = "advertising {ended}"
      await sleep(1ms)

      // The central.
      let s = try await bluetooth.request()
      step = "state {s}"
      await sleep(1ms)
      for await a in bluetooth.scan(services: ["180d"]) {
        step = "heard {a.device.id} {a.device.name} {a.rssi} {a.localName} {a.services.joined(separator: ",")} {a.connectable} {a.time.unixNano() > 0}"
        break
      }
      await sleep(1ms)
      let c = bluetooth.connect(bluetooth.Device(id: "AA:BB:CC:DD:EE:FF"), timeout: 10s)
      let svcs = try await c.services()
      step = "services {svcs.map(x => "{x.uuid}:{x.characteristics.map(ch => "{ch.uuid}/{ch.properties.count()}/{ch.descriptors.joined(separator: ",")}").joined(separator: ";")}").joined(separator: " ")}"
      await sleep(1ms)
      let ch = try await c.characteristic("180d", "2a37")
      let v = try await ch.read()
      step = "read {v.hex(" ")}"
      await sleep(1ms)
      try await ch.write(bytes([7, 0]))
      step = "mtu {try await c.mtu()}"
      await sleep(1ms)
      do {
        for await n in ch.notifications() {
          step = "notified {n.hex(" ")}"
          await sleep(1ms)
          try await ch.write(bytes([0xdd]))
        }
      } catch e {
        while c.state != .disconnected {
          await sleep(5ms)
        }
        step = "lost {c.error?.message ?? "-"}"
        await sleep(1ms)
        step = "subscription ended {e.message}"
        await sleep(1ms)
      }
      do {
        let _ = try await ch.read()
      } catch e {
        step = "read after: {e.message}"
        await sleep(1ms)
      }
      while try await bluetooth.request() != .off {
        await sleep(5ms)
      }
      step = "off"
      await sleep(1ms)
      do {
        for await _ in bluetooth.scan() {
        }
      } catch e {
        step = "scan while off: {e.message}"
        await sleep(1ms)
      }
      do {
        let _ = try await bluetooth.choose([bluetooth.Filter(services: ["180d"])])
      } catch e {
        step = "choose: {e.message}"
        await sleep(1ms)
      }
    } catch e {
      step = "fail {e.message}"
      return
    }
    step = "done"
  }
}

app { Probe() }
`
