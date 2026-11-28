// The JavaScript encoders of a Video's reports agree with Go's
// (ops/video.go): they write ops/testdata/videostate.bin and
// ops/testdata/videotracks.bin.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { videoState, videoTracks, VideoFlags } from "../src/video.js";

const testdata = fileURLToPath(new URL("../../../ops/testdata/", import.meta.url));

test("writes the golden reports", () => {
  const f = VideoFlags.Ready | VideoFlags.Playing | VideoFlags.PictureInPicture;
  assert.deepEqual(videoState(f, 12.5, 634.566, 1.5, 1920, 1080), new Uint8Array(readFileSync(testdata + "videostate.bin")));
  const ts = [
    { subtitle: true, selected: false, id: "0", label: "English", language: "en" },
    { subtitle: true, selected: true, id: "https://example.com/fr.vtt", label: "Français", language: "fr" },
    { subtitle: false, selected: true, id: "a1", label: "日本語", language: "ja" },
  ];
  assert.deepEqual(videoTracks(ts), new Uint8Array(readFileSync(testdata + "videotracks.bin")));
});
