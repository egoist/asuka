package dev.asuka

import android.content.Context
import android.util.Log
import java.io.File

/** The core's JNI entry points (core/src/abi/jni_android.c). Main thread. */
object AsukaNative {
    @JvmStatic external fun start(runtime: AsukaRuntime)
    @JvmStatic external fun attach()
    @JvmStatic external fun event(bytes: ByteArray)
    @JvmStatic external fun listCount(list: Int): Long
    @JvmStatic external fun listBind(list: Int, index: Long, cell: Int)
    @JvmStatic external fun listUnbind(list: Int, cell: Int)
    @JvmStatic external fun runPosted()
    @JvmStatic external fun setenv(key: String, value: String)
    @JvmStatic external fun crashStart(info: String): String
    @JvmStatic external fun crashNotify()
    /** Runs the app compiled into the library: null, or what went wrong. */
    @JvmStatic external fun run(): String?
    /** Connects to asuka dev, which sends the app's bytecode: null, or what went wrong. */
    @JvmStatic external fun devConnect(host: String, port: Int, hello: String): String?

    /**
     * Starts the app, once the core has: under asuka dev (a debuggable
     * build with files/asuka/dev, host:port, which the CLI writes), the
     * bytecode the CLI sends; otherwise the app compiled into the library.
     */
    fun startApp(context: Context) {
        val debuggable = context.applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE != 0
        val dev = File(context.filesDir, "asuka/dev")
        val err = if (debuggable && dev.exists()) {
            val (host, port) = dev.readText().trim().split(":").let { it[0] to it[1].toInt() }
            val hello = """{"platform":"android","device":"${android.os.Build.MODEL}","sdk":${android.os.Build.VERSION.SDK_INT}}"""
            devConnect(host, port, hello)
        } else {
            run()
        }
        if (err != null) Log.e("asuka", err)
    }

    private var loaded = false
    private var loader: Thread? = null

    /**
     * Starts loading the core on a thread of its own, as the app starts:
     * mapping the library and starting Go's runtime then overlap with the
     * Activity's own start, rather than waiting for its first frame.
     */
    fun preload(context: Context) {
        if (loader != null || loaded) return
        val app = context.applicationContext
        loader = Thread({ open(app) }, "asuka-load").apply { start() }
    }

    /** Loads the core, or waits for preload to have. */
    fun load(context: Context) {
        if (loaded) return
        loaded = true
        loader?.let { it.join(); return }
        open(context)
    }

    /**
     * In development `asuka dev` pushes the app's library to
     * files/asuka/libasuka.so, so a change needs no reinstall; otherwise (and
     * always in a release build) it's packaged with the app.
     */
    private fun open(context: Context) {
        val debuggable = context.applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE != 0
        val dev = File(context.filesDir, "asuka/libasuka.so")
        if (debuggable && dev.exists()) {
            Log.i("asuka", "loading ${dev.path}")
            System.load(dev.absolutePath)
        } else {
            System.loadLibrary("asuka")
        }
        // The app's directory (std/fs's appDir, where the crash record is
        // kept too): the core reads ASUKA_DATA. A library from before
        // asuka_setenv hasn't it: nothing to set then.
        runCatching {
            setenv("ASUKA_DATA", context.filesDir.absolutePath)
            // Its other directories (std/fs's configDir, cacheDir, logDir):
            // settings with its files, caches in Context.getCacheDir, which
            // the system empties when it needs the room.
            setenv("ASUKA_CONFIG", context.filesDir.absolutePath)
            setenv("ASUKA_CACHE", context.cacheDir.absolutePath)
            setenv("ASUKA_LOGS", File(context.filesDir, "logs").absolutePath)
        }
        // platform/crash's record of the run, before the app's init.
        AsukaCrash.start(context)
    }
}
