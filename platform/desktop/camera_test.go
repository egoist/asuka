package desktop_test

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/egoist/asuka/lang/compile"
	"github.com/egoist/asuka/lang/target"
)

// platform/camera's Linux C on the test host: a camera of GStreamer's test
// pattern with a QR code drawn on it (ASUKA_CAMERA_SOURCE), shown in a
// View, photographed, recorded with and without sound (a tone for the
// microphone), its code read by zbar; take's dialog shown and cancelled;
// and with no camera, the View's error and take's. What can't run here:
// the camera portal and a real camera (PipeWire, V4L2), and take's photo
// (its button needs a click).
func TestStdLinuxCamera(t *testing.T) {
	if runtime.GOOS != "linux" {
		t.Skip("the standard library's Linux C (run in the asuka-linux-test image)")
	}
	exe := stdHost(t)
	run := func(t *testing.T, src string, env ...string) string {
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
	expect := func(t *testing.T, got string, want ...string) {
		for _, w := range want {
			if !strings.Contains(got, w) {
				t.Errorf("no %q in:\n%s", w, got)
			}
		}
	}

	t.Run("shown", func(t *testing.T) {
		got := run(t, `import platform/camera
import std/fs

async fn stopSoon() -> bool {
  await sleep(1s)
  camera.stopRecording()
  return true
}

fn size(path: string) -> int {
  if let e = try? fs.stat(path) {
    return e.size
  }
  return 0
}

component Probe {
  state step = "start"
  state showing = false
  state video = false
  Text("#{step}")
  if showing {
    camera.View {
      mode: if video { .video } else { .photo }
      scan: true
      codes: ["qr"]
      on code(c) { step = "code {c.kind} {c.value}" }
      on error(m) { step = "error {m}" }
    }
  }
  task {
    do {
      do {
        let _ = try await camera.capture()
        step = "fail: a photo with no view"
      } catch e {
        step = "no view: {e.message}"
      }
      await sleep(1ms)
      showing = true
      await sleep(1500ms)
      let p = try await camera.capture()
      step = "photo {p.hasSuffix(".jpg")} {size(p) > 1000}"
      await sleep(1ms)
      do {
        let _ = try await camera.record()
      } catch e {
        step = "photo mode: {e.message}"
        await sleep(1ms)
      }
      video = true
      await sleep(800ms)
      let v = try await camera.record(mute: true, maxDuration: 1s)
      step = "video {v.hasSuffix(".mp4")} {size(v) > 1000}"
      await sleep(1ms)
      let (s, _) = try await all(camera.record(), stopSoon())
      step = "sound {s.hasSuffix(".mp4")} {size(s) > 1000} {s != v}"
      await sleep(1ms)
      do {
        try await withTimeout(800ms) {
          let _ = try await camera.take()
        }
      } catch e {
        step = "take cancelled"
        await sleep(1ms)
      }
      showing = false
      await sleep(100ms)
      step = "done"
    } catch e {
      step = "fail {e.message}"
    }
  }
}

app { Probe() }
`, "ASUKA_CAMERA_SOURCE=videotestsrc is-live=true pattern=white ! video/x-raw,width=640,height=480,framerate=30/1 ! qroverlay data=asuka",
			"ASUKA_CAMERA_SOUND=audiotestsrc is-live=true")
		expect(t, got, "host: native view platform/camera#View", "host: no view: CameraError.noView", "host: code qr ASUKA",
			"host: photo true true", "host: photo mode: the camera view isn't in video mode", "host: video true true",
			"host: sound true true true", "host: take cancelled", "host: done")
		if strings.Contains(got, "host: error") {
			t.Errorf("the view failed:\n%s", got)
		}
	})

	t.Run("absent", func(t *testing.T) {
		got := run(t, `import platform/camera

component Probe {
  state step = "start"
  state failed = ""
  Text("#{step}")
  camera.View {
    on error(m) { failed = m }
  }
  task {
    while failed == "" {
      await sleep(10ms)
    }
    step = "view {failed}"
    await sleep(1ms)
    do {
      let _ = try await camera.take()
      step = "fail: took"
    } catch camera.CameraError.noCamera {
      step = "take: no camera"
    } catch e {
      step = "fail {e.message}"
    }
    await sleep(1ms)
    step = "done"
  }
}

app { Probe() }
`, "ASUKA_CAMERA_SOURCE=")
		expect(t, got, "host: view no camera", "host: take: no camera", "host: done")
	})
}
