// platform/nowplaying on Android: a MediaSession, which the notification
// shade's media controls, the lock screen, Bluetooth headsets, Android
// Auto and Wear OS show and drive, with its media-style notification.
// While playing, MediaService is a foreground service of the media
// playback type, so the app plays on out of sight; paused, the
// notification stays, and can be swiped away. On the main thread.
package platform.nowplaying

import android.app.ForegroundServiceStartNotAllowedException
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.media.MediaMetadata
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import androidx.annotation.RequiresApi
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import dev.asuka.AsukaImages
import dev.asuka.AsukaMediaSession
import dev.asuka.AsukaModules
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private const val CHANNEL = "asuka.media"
private const val NOTIFICATION = 0x6e76_0002
private const val SKIP_FORWARD = "asuka.nowplaying.skipForward"
private const val SKIP_BACKWARD = "asuka.nowplaying.skipBackward"

/** The notification channel, made once. */
@RequiresApi(26)
private fun channel(ctx: Context): String {
    val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
    if (nm.getNotificationChannel(CHANNEL) == null) {
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Now playing", NotificationManager.IMPORTANCE_LOW).apply { setShowBadge(false) })
    }
    return CHANNEL
}

/** A notification on the channel. Before Android 8 there are no channels: its own priority, low as the channel's importance. */
private fun builder(ctx: Context): Notification.Builder =
    if (Build.VERSION.SDK_INT >= 26) Notification.Builder(ctx, channel(ctx))
    else @Suppress("DEPRECATION") Notification.Builder(ctx).setPriority(Notification.PRIORITY_LOW)

private fun Info.has(c: Command) = commands and (1L shl c.value.toInt()) != 0L

/** The PlaybackState actions of the commands enabled. */
private fun actions(i: Info): Long {
    var a = 0L
    if (i.has(Command.PLAY) || i.has(Command.TOGGLE_PLAY_PAUSE)) a = a or PlaybackState.ACTION_PLAY or PlaybackState.ACTION_PLAY_PAUSE
    if (i.has(Command.PAUSE) || i.has(Command.TOGGLE_PLAY_PAUSE)) a = a or PlaybackState.ACTION_PAUSE or PlaybackState.ACTION_PLAY_PAUSE
    if (i.has(Command.STOP)) a = a or PlaybackState.ACTION_STOP
    if (i.has(Command.NEXT)) a = a or PlaybackState.ACTION_SKIP_TO_NEXT
    if (i.has(Command.PREVIOUS)) a = a or PlaybackState.ACTION_SKIP_TO_PREVIOUS
    if (i.has(Command.SEEK)) a = a or PlaybackState.ACTION_SEEK_TO
    if (i.has(Command.SKIP_FORWARD)) a = a or PlaybackState.ACTION_FAST_FORWARD
    if (i.has(Command.SKIP_BACKWARD)) a = a or PlaybackState.ACTION_REWIND
    return a
}

internal object MediaCenter : Center {
    private val app get() = AsukaModules.context
    private var session: MediaSession? = null
    private var info: Info? = null
    private var artSource = ""
    private var art: Bitmap? = null
    private var service = false // MediaService was started
    private val watchers = ArrayList<(Command, Double) -> kotlin.Unit>()

    private fun command(c: Command, v: Double) {
        val i = info ?: return
        if (!i.has(c)) return
        for (w in watchers.toList()) w(c, v)
    }

    private val callback = object : MediaSession.Callback() {
        override fun onPlay() = command(Command.PLAY, 0.0)
        override fun onPause() = command(Command.PAUSE, 0.0)
        override fun onStop() = command(Command.STOP, 0.0)
        override fun onSkipToNext() = command(Command.NEXT, 0.0)
        override fun onSkipToPrevious() = command(Command.PREVIOUS, 0.0)
        override fun onSeekTo(pos: Long) = command(Command.SEEK, pos / 1000.0)
        override fun onFastForward() = command(Command.SKIP_FORWARD, info?.skip ?: 0.0)
        override fun onRewind() = command(Command.SKIP_BACKWARD, info?.skip ?: 0.0)
        override fun onCustomAction(action: String, extras: Bundle?) {
            when (action) {
                SKIP_FORWARD -> command(Command.SKIP_FORWARD, info?.skip ?: 0.0)
                SKIP_BACKWARD -> command(Command.SKIP_BACKWARD, info?.skip ?: 0.0)
            }
        }
    }

    // The app's session (the host's, which platform/car's media browser
    // hands Android Auto too), with this package's callback.
    private fun session(): MediaSession {
        session?.let { return it }
        val s = AsukaMediaSession.get(app)
        AsukaMediaSession.addCallback(callback)
        app.packageManager.getLaunchIntentForPackage(app.packageName)?.let {
            s.setSessionActivity(PendingIntent.getActivity(app, 0, it, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT))
        }
        session = s
        return s
    }

    override fun set(info: Info) {
        this.info = info
        val s = session()
        val state = when (info.state) {
            State.PLAYING -> PlaybackState.STATE_PLAYING
            State.STOPPED -> PlaybackState.STATE_STOPPED
            else -> PlaybackState.STATE_PAUSED
        }
        val ps = PlaybackState.Builder()
            .setActions(actions(info))
            // The system advances the position from now at the rate, while playing.
            .setState(state, (info.position * 1000).toLong(), if (info.rate > 0) info.rate.toFloat() else 1f, SystemClock.elapsedRealtime())
        // Skips fill the slots of previous and next when the app has none.
        if (info.has(Command.SKIP_BACKWARD)) ps.addCustomAction(PlaybackState.CustomAction.Builder(SKIP_BACKWARD, "Skip back", android.R.drawable.ic_media_rew).build())
        if (info.has(Command.SKIP_FORWARD)) ps.addCustomAction(PlaybackState.CustomAction.Builder(SKIP_FORWARD, "Skip forward", android.R.drawable.ic_media_ff).build())
        s.setPlaybackState(ps.build())
        if (info.artwork != artSource) {
            artSource = info.artwork
            art = null
            if (info.artwork.isNotEmpty()) {
                val src = info.artwork
                AsukaImages.load(app, src) { r ->
                    if (artSource == src && this.info != null) {
                        art = r.getOrNull()?.bitmap
                        metadata()
                        notifyShown()
                    }
                }
            }
        }
        metadata()
        s.isActive = true
        if (info.state == State.PLAYING) startForeground() else stopForeground()
        notifyShown()
    }

    private fun metadata() {
        val i = info ?: return
        val m = MediaMetadata.Builder()
            .putString(MediaMetadata.METADATA_KEY_TITLE, i.title)
            .putString(MediaMetadata.METADATA_KEY_ARTIST, i.artist)
            .putString(MediaMetadata.METADATA_KEY_ALBUM, i.album)
        if (i.duration > 0) m.putLong(MediaMetadata.METADATA_KEY_DURATION, (i.duration * 1000).toLong())
        art?.let { m.putBitmap(MediaMetadata.METADATA_KEY_ART, it) }
        session?.setMetadata(m.build())
    }

    private fun manager() = app.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    /** Whether it plays: MediaService stays in the foreground only then. */
    val playing get() = info?.state == State.PLAYING

    /** The media notification of what shows, or null. */
    fun notification(ctx: Context): Notification? {
        val i = info ?: return null
        val s = session ?: return null
        val b = builder(ctx)
            .setSmallIcon(ctx.applicationInfo.icon)
            .setContentTitle(i.title)
            .setContentText(i.artist)
            .setStyle(Notification.MediaStyle().setMediaSession(s.sessionToken))
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setCategory(Notification.CATEGORY_TRANSPORT)
            .setOnlyAlertOnce(true)
            .setOngoing(i.state == State.PLAYING)
        // Before Android 12 a foreground service's notification shows at once.
        if (Build.VERSION.SDK_INT >= 31) b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE)
        if (i.album.isNotEmpty()) b.setSubText(i.album)
        art?.let { b.setLargeIcon(it) }
        s.controller.sessionActivity?.let { b.setContentIntent(it) }
        return b.build()
    }

    /** Posts the notification again, as what shows changed. */
    private fun notifyShown() {
        val n = notification(app) ?: return
        runCatching { manager().notify(NOTIFICATION, n) }
    }

    private fun startForeground() {
        if (service) return
        try {
            ContextCompat.startForegroundService(app, Intent(app, MediaService::class.java))
            service = true
        } catch (e: IllegalStateException) {
            // Android 12 and later, in the background without an exemption:
            // the notification alone, as Media3 does.
            if (Build.VERSION.SDK_INT < 31 || e !is ForegroundServiceStartNotAllowedException) throw e
        } catch (_: SecurityException) {
            // Not declared.
        }
    }

    /** MediaService left the foreground by itself (paused as it started). */
    fun serviceStopped() { service = false }

    private fun stopForeground() {
        if (!service) return
        service = false
        MediaService.running?.let {
            it.stopForeground(Service.STOP_FOREGROUND_DETACH)
            it.stopSelf()
        }
    }

    override fun clear() {
        info = null
        artSource = ""
        art = null
        if (service) {
            service = false
            MediaService.running?.let {
                it.stopForeground(Service.STOP_FOREGROUND_REMOVE)
                it.stopSelf()
            }
        }
        manager().cancel(NOTIFICATION)
        // The session stays, the app's for as long as it runs (Android Auto
        // may hold its token): it shows nothing now.
        session?.let {
            it.setMetadata(null)
            it.setPlaybackState(PlaybackState.Builder().setState(PlaybackState.STATE_NONE, 0, 0f).build())
            it.isActive = false
        }
        session = null
    }

    override suspend fun watch(command: (Command, Double) -> kotlin.Unit) {
        withContext(Dispatchers.Main) { watchers.add(command) }
        try {
            awaitCancellation()
        } finally {
            Handler(Looper.getMainLooper()).post { watchers.remove(command) }
        }
    }
}

/** The foreground service that keeps the app playing out of sight. */
class MediaService : Service() {
    companion object {
        var running: MediaService? = null
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        running = this
    }

    override fun onDestroy() {
        if (running === this) running = null
        super.onDestroy()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Started for the foreground, it must go there, even if playback
        // paused or went meanwhile; then it leaves it at once.
        val n = MediaCenter.notification(this)
            ?: builder(this).setSmallIcon(applicationInfo.icon).build()
        ServiceCompat.startForeground(this, NOTIFICATION, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        if (!MediaCenter.playing) {
            stopForeground(if (MediaCenter.notification(this) != null) STOP_FOREGROUND_DETACH else STOP_FOREGROUND_REMOVE)
            stopSelf()
            MediaCenter.serviceStopped()
        }
        return START_NOT_STICKY
    }
}

fun makeCenter(): Center = MediaCenter
