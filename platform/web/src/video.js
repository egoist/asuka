// Video: a <video> with the browser's controls, picture in picture and
// full screen. It reports its natural size (Loaded), its end, its errors,
// its playback (VideoState: as it changes, and every TimeInterval while it
// plays) and its tracks (VideoTracks: the <video>'s text tracks, its
// SubtitleFiles' <track>s among them, and its audio tracks where the
// browser has them, as Safari does). A <video> sends no headers of the
// app's: Headers is ignored.

import { KindProp as P, Event, Events, Command, Reader, Writer } from "./ops.js";

/** VideoState's flags (ops/video.go). */
export const VideoFlags = Object.freeze({ Ready: 1, Playing: 2, Buffering: 4, Ended: 8, PictureInPicture: 16, FullScreen: 32 });

/** videoState is VideoState's payload (ops.AppendVideoState). */
export function videoState(flags, time, duration, rate, width, height) {
  return new Writer().u8(flags).f64(time).f64(duration).f32(rate).f32(width).f32(height).done();
}

/** videoTracks is VideoTracks' payload: tracks are {subtitle, selected, id, label, language}. */
export function videoTracks(tracks) {
  const w = new Writer().u32(tracks.length);
  for (const t of tracks) w.u8(t.subtitle ? 0 : 1).u8(t.selected ? 1 : 0).string(t.id).string(t.label).string(t.language);
  return w.done();
}

const fits = ["contain", "cover", "fill", "none"];

/** trackID is a text track's id: its <track>'s (a SubtitleFiles file's URL), else its place. */
function trackID(t, i) { return t.id || String(i); }

/** makeVideo is the Video kind: sourceURL resolves a source, own gives a node its element, decodeStrings reads a list of strings (media.js's, which import what imports this). */
export function makeVideo(sourceURL, own, decodeStrings) {
  /** report sends VideoState, unless only the time changed and force is false. */
  function report(h, n, force) {
    const v = n.video;
    let f = 0;
    const ready = v.readyState >= 1 && !!v.currentSrc;
    if (ready) f |= VideoFlags.Ready;
    if (!v.paused && !v.ended) {
      f |= VideoFlags.Playing;
      if (n.waiting || v.readyState < 3) f |= VideoFlags.Buffering;
    }
    if (v.ended && !v.loop) f |= VideoFlags.Ended;
    if (document.pictureInPictureElement === v) f |= VideoFlags.PictureInPicture;
    if (document.fullscreenElement === v || document.webkitFullscreenElement === v) f |= VideoFlags.FullScreen;
    const dur = ready ? v.duration : NaN;
    const rate = v.playbackRate || 1;
    const key = `${f}|${dur}|${rate}|${v.videoWidth}x${v.videoHeight}`;
    if (!force && key === n.reported) return;
    n.reported = key;
    const s = videoState(f, v.currentTime || 0, dur, rate, v.videoWidth, v.videoHeight);
    h.emit(n, Event.VideoState, (id) => Events.videoState(id, s));
  }

  function reportTracks(h, n) {
    const v = n.video, ts = [];
    for (let i = 0; i < v.textTracks.length; i++) {
      const t = v.textTracks[i];
      if (t.kind !== "subtitles" && t.kind !== "captions") continue;
      ts.push({ subtitle: true, selected: t.mode === "showing", id: trackID(t, i), label: t.label, language: t.language });
    }
    const a = v.audioTracks;
    for (let i = 0; a && i < a.length; i++) {
      ts.push({ subtitle: false, selected: a[i].enabled, id: a[i].id || String(i), label: a[i].label, language: a[i].language });
    }
    const key = JSON.stringify(ts);
    if (key === n.tracksKey) return;
    n.tracksKey = key;
    const b = videoTracks(ts);
    h.emit(n, Event.VideoTracks, (id) => Events.videoTracks(id, b));
  }

  /** applyTracks shows the app's subtitle (and plays its audio track), once they're there. */
  function applyTracks(n) {
    const v = n.video;
    if (n.subtitle != null) {
      for (let i = 0; i < v.textTracks.length; i++) {
        const t = v.textTracks[i];
        if (t.kind !== "subtitles" && t.kind !== "captions") continue;
        const want = trackID(t, i) === n.subtitle ? "showing" : "disabled";
        if (t.mode !== want) t.mode = want;
      }
    }
    const a = v.audioTracks;
    if (n.audioTrack != null && a) {
      let found = false;
      for (let i = 0; i < a.length; i++) if ((a[i].id || String(i)) === n.audioTrack) found = true;
      if (found) for (let i = 0; i < a.length; i++) a[i].enabled = (a[i].id || String(i)) === n.audioTrack;
    }
  }

  function setTick(h, n) {
    clearInterval(n.tick);
    n.tick = 0;
    const v = n.video;
    if (n.interval > 0 && !v.paused && !v.ended) n.tick = setInterval(() => report(h, n, true), n.interval * 1000);
  }

  /** setFiles puts the SubtitleFiles' <track>s in the <video>. */
  function setFiles(h, n, files) {
    const v = n.video;
    for (const t of [...v.querySelectorAll("track")]) t.remove();
    // A <track> from another site loads only with CORS: the <video>'s.
    if (files.some((f, i) => i % 3 === 0 && /^https?:/.test(f) && new URL(f).origin !== location.origin)) v.crossOrigin = "anonymous";
    for (let i = 0; i + 2 < files.length; i += 3) {
      const t = document.createElement("track");
      t.kind = "subtitles";
      t.id = files[i];
      t.src = sourceURL(h, files[i]);
      if (files[i + 1]) t.srclang = files[i + 1];
      t.label = files[i + 2] || files[i + 1] || "";
      v.appendChild(t);
    }
    applyTracks(n);
  }

  return {
    create(h, n) {
      const el = own(n, document.createElement("div"));
      el.className = "asuka-video";
      const v = document.createElement("video");
      v.playsInline = true;
      v.controls = true;
      v.preload = "metadata";
      v.disablePictureInPicture = true; // until PictureInPicture
      el.appendChild(v);
      n.video = v;
      n.interval = 0;
      n.startAt = 0;
      v.addEventListener("loadedmetadata", () => {
        n.natural = [v.videoWidth, v.videoHeight];
        h.emit(n, Event.Loaded, (id) => Events.loaded(id, v.videoWidth, v.videoHeight));
        if (n.pendingStart > 0) {
          v.currentTime = n.pendingStart;
          n.pendingStart = 0;
        }
        applyTracks(n);
        reportTracks(h, n);
        report(h, n, false);
      });
      v.addEventListener("ended", () => {
        h.emit(n, Event.Ended, Events.ended);
        report(h, n, true);
      });
      v.addEventListener("error", () => {
        if (!v.getAttribute("src")) return;
        h.emit(n, Event.Failed, (id) => Events.failed(id, v.error?.message || "the video couldn't play"));
      });
      v.addEventListener("waiting", () => { n.waiting = true; report(h, n, false); });
      v.addEventListener("playing", () => { n.waiting = false; setTick(h, n); report(h, n, true); });
      for (const e of ["play", "pause", "canplay", "durationchange", "ratechange", "enterpictureinpicture", "leavepictureinpicture", "resize"]) {
        v.addEventListener(e, () => { setTick(h, n); report(h, n, e === "pause"); });
      }
      v.addEventListener("seeked", () => report(h, n, true));
      v.addEventListener("fullscreenchange", () => report(h, n, false));
      v.addEventListener("webkitfullscreenchange", () => report(h, n, false));
      for (const list of [v.textTracks, v.audioTracks]) {
        if (!list) continue;
        list.addEventListener("addtrack", () => { applyTracks(n); reportTracks(h, n); });
        list.addEventListener("removetrack", () => reportTracks(h, n));
        list.addEventListener("change", () => reportTracks(h, n));
      }
    },
    prop(h, n, p, val) {
      const v = n.video;
      switch (p) {
        case P.Video.Source:
          n.reported = "";
          n.tracksKey = "";
          n.pendingStart = n.startAt;
          if (val) v.src = sourceURL(h, val);
          else { v.removeAttribute("src"); v.load(); }
          report(h, n, true);
          break;
        case P.Video.Mode: v.style.objectFit = fits[val] || "contain"; break;
        case P.Video.HideControls: v.controls = !val; break;
        case P.Video.AutoPlay: v.autoplay = !!val; if (val && v.getAttribute("src")) v.play().catch(() => {}); break;
        case P.Video.Loop: v.loop = !!val; break;
        case P.Video.Muted: v.muted = !!val; break;
        case P.Video.Rate: v.defaultPlaybackRate = v.playbackRate = val > 0 ? val : 1; break;
        case P.Video.Volume: v.volume = Math.min(1, Math.max(0, val)); break;
        case P.Video.Poster: v.poster = val ? sourceURL(h, val) : ""; break;
        case P.Video.StartAt: n.startAt = val; break;
        case P.Video.TimeInterval: n.interval = val; setTick(h, n); break;
        case P.Video.Subtitle: n.subtitle = val; applyTracks(n); break;
        case P.Video.AudioTrack: n.audioTrack = val; applyTracks(n); break;
        case P.Video.SubtitleFiles: setFiles(h, n, decodeStrings(val)); break;
        case P.Video.PictureInPicture: v.disablePictureInPicture = !val; break;
        case P.Video.AutoPictureInPicture: v.autoPictureInPicture = !!val; break;
        // Headers: a <video> sends none of the app's.
      }
    },
    command(h, n, cmd, args) {
      const v = n.video;
      switch (cmd) {
        case Command.Play: v.play().catch(() => {}); return true;
        case Command.Pause: v.pause(); return true;
        case Command.Seek: v.currentTime = new Reader(args).f64(); return true;
        case Command.VideoAction:
          switch (args[0]) {
            case 0: if (!v.disablePictureInPicture && document.pictureInPictureEnabled) v.requestPictureInPicture?.().catch(() => {}); break;
            case 1: if (document.pictureInPictureElement === v) document.exitPictureInPicture().catch(() => {}); break;
            case 2: (v.requestFullscreen?.() ?? v.webkitEnterFullscreen?.())?.catch?.(() => {}); break;
            case 3: if (document.fullscreenElement === v) document.exitFullscreen().catch(() => {}); break;
          }
          return true;
      }
      return false;
    },
    destroy(h, n) {
      clearInterval(n.tick);
      const v = n.video;
      if (document.pictureInPictureElement === v) document.exitPictureInPicture().catch(() => {});
      v.pause();
      v.removeAttribute("src");
      v.load();
      return false;
    },
    measure(h, n) { return n.natural || [0, 0]; },
  };
}
