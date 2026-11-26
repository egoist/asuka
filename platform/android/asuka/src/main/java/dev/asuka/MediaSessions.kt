package dev.asuka

import android.content.Context
import android.media.session.MediaSession
import android.os.Bundle
import android.os.Handler
import android.os.Looper

/**
 * The app's one MediaSession, made as a package first asks for it and
 * kept for the process: platform/nowplaying shows what plays with it (the
 * notification shade, the lock screen, headsets), and platform/car's media
 * browser hands its token to Android Auto, which can be given a token
 * only once. Its callback calls each package's (addCallback), on the main
 * thread.
 */
object AsukaMediaSession {
    private var session: MediaSession? = null
    private val callbacks = ArrayList<MediaSession.Callback>()

    /** The session; main thread. */
    fun get(context: Context): MediaSession {
        session?.let { return it }
        val app = context.applicationContext
        val s = MediaSession(app, "asuka")
        s.setCallback(object : MediaSession.Callback() {
            private fun each(f: MediaSession.Callback.() -> kotlin.Unit) { for (c in callbacks.toList()) c.f() }
            override fun onPlay() = each { onPlay() }
            override fun onPause() = each { onPause() }
            override fun onStop() = each { onStop() }
            override fun onSkipToNext() = each { onSkipToNext() }
            override fun onSkipToPrevious() = each { onSkipToPrevious() }
            override fun onSeekTo(pos: Long) = each { onSeekTo(pos) }
            override fun onFastForward() = each { onFastForward() }
            override fun onRewind() = each { onRewind() }
            override fun onCustomAction(action: String, extras: Bundle?) = each { onCustomAction(action, extras) }
            override fun onPlayFromMediaId(mediaId: String?, extras: Bundle?) = each { onPlayFromMediaId(mediaId, extras) }
            override fun onPlayFromSearch(query: String?, extras: Bundle?) = each { onPlayFromSearch(query, extras) }
            override fun onSkipToQueueItem(id: Long) = each { onSkipToQueueItem(id) }
        }, Handler(Looper.getMainLooper()))
        session = s
        return s
    }

    /** Hears the session's commands; main thread. */
    fun addCallback(c: MediaSession.Callback) {
        if (c !in callbacks) callbacks.add(c)
    }

    fun removeCallback(c: MediaSession.Callback) {
        callbacks.remove(c)
    }
}
