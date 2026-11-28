package dev.asuka

import org.junit.Assert.assertArrayEquals
import org.junit.Test
import java.io.File

// The Kotlin encoders of a Video's reports agree with Go's (ops/video.go):
// they write ops/testdata/videostate.bin and ops/testdata/videotracks.bin.
class VideoReportTest {
    private val testdata = File("../../../ops/testdata")

    @Test
    fun encodesTheGoldenReports() {
        val flags = AsukaVideoReport.READY or AsukaVideoReport.PLAYING or AsukaVideoReport.PIP
        assertArrayEquals(File(testdata, "videostate.bin").readBytes(), AsukaVideoReport.state(flags, 12.5, 634.566, 1.5f, 1920f, 1080f))
        val ts = listOf(
            AsukaVideoReport.Track(true, false, "0", "English", "en"),
            AsukaVideoReport.Track(true, true, "https://example.com/fr.vtt", "Français", "fr"),
            AsukaVideoReport.Track(false, true, "a1", "日本語", "ja"),
        )
        assertArrayEquals(File(testdata, "videotracks.bin").readBytes(), AsukaVideoReport.tracks(ts))
    }
}
