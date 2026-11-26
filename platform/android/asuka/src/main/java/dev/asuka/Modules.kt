package dev.asuka

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** A native module's method: the call's bytes, and a reply (once). */
typealias AsukaModuleMethod = (args: ByteArray, reply: (Result<ByteArray>) -> Unit) -> Unit

/** Registers a module's methods for ui.Call: an app's, alongside the host's own. */
fun AsukaRuntime.registerModule(name: String, methods: Map<String, AsukaModuleMethod>) {
    modules[name] = methods
}

/** Runs a Call command and answers with a Result event. */
fun AsukaRuntime.call(root: Int, args: ByteBuffer) {
    if (args.remaining() < 12) return
    val b = args.order(ByteOrder.LITTLE_ENDIAN)
    val id = b.int
    fun str(): String { val a = ByteArray(b.int); b.get(a); return String(a, Charsets.UTF_8) }
    val module = str()
    val method = str()
    val payload = ByteArray(b.remaining())
    b.get(payload)
    builtins()
    var answered = false
    val reply: (Result<ByteArray>) -> Unit = { res ->
        onMain {
            if (!answered) {
                answered = true
                val out = ByteBuffer.allocate(5 + (res.getOrNull()?.size ?: 0) + (res.exceptionOrNull()?.message?.length ?: 0) * 4)
                    .order(ByteOrder.LITTLE_ENDIAN)
                out.putInt(id)
                res.fold({ out.put(1); out.put(it) }, { out.put(0); out.put((it.message ?: "error").toByteArray()) })
                val bytes = out.array().copyOf(out.position())
                // An answer, not an event the root listens for: sent as is.
                send(AsukaEvents.result(root, bytes))
            }
        }
    }
    val m = modules[module]?.let { it[method] ?: it["*"] } ?: return reply(Result.failure(Exception("no module method $module.$method")))
    try {
        m(payload, reply)
    } catch (e: Exception) {
        reply(Result.failure(e))
    }
}

/**
 * The host's own modules, by name: what the platform SDK's packages
 * over platform/host call (notify, push, the library...),
 * and what the core calls itself (web: a WebView's scripts and cookies;
 * share: ui.ShareFiles). What a std native package implements is that
 * package's (std/<name>/android), in the apps that import it.
 */
private fun AsukaRuntime.registerBuiltinModules() {
    modules["web"] = webMethods()
    modules["share"] = shareMethods()
    modules["view"] = captureMethods()
    modules["library"] = libraryMethods()
    modules["photoLibrary"] = AsukaPhotoLibrary(this).methods()
    modules["background"] = AsukaBackground(this).methods()
    modules["notify"] = notifyMethods()
    modules["push"] = pushMethods()
    modules["periodic"] = AsukaPeriodicHub.methods(this)
    modules["contacts"] = contactsMethods()
    modules["calendar"] = calendarMethods()
    modules["compose"] = composeMethods()
    modules["widget"] = widgetMethods()
    modules["liveActivity"] = liveActivityMethods()
    modules["dock"] = dockMethods()
    // platform/recent and platform/login: a desktop's. Android's file pickers keep
    // their own recent files, and an app doesn't start at login.
    val answer = { b: ByteArray -> { _: ByteArray, reply: (Result<ByteArray>) -> Unit -> reply(Result.success(b)) } }
    modules["recent"] = mapOf("add" to answer(ByteArray(0)), "clear" to answer(ByteArray(0)), "list" to answer("[]".toByteArray()))
    modules["login"] = mapOf(
        "set" to { _: ByteArray, reply: (Result<ByteArray>) -> Unit -> reply(Result.failure(Exception("not available: an Android app doesn't start at login"))) },
        "enabled" to answer(byteArrayOf(0)),
        "openedAtLogin" to answer(byteArrayOf(0)),
    )
    // platform/icloud: iCloud is Apple's.
    modules["icloud"] = mapOf(
        "stop" to { _, reply -> reply(Result.success(ByteArray(0))) },
        "*" to { _, reply -> reply(Result.failure(Exception("not available: iCloud is Apple's"))) },
    )
}

/** platform/dock: a phone has no Dock or taskbar: nothing to show, nothing asked. */
fun dockMethods(): Map<String, AsukaModuleMethod> = mapOf(
    "progress" to { _, reply -> reply(Result.success(ByteArray(0))) },
    "attention" to { _, reply -> reply(Result.success(ByteArray(4))) },
    "cancelAttention" to { _, reply -> reply(Result.success(ByteArray(0))) },
)

/**
 * An optional module's (platform/android/asuka-<name>: Firebase's
 * messaging, WorkManager), which only the apps that import its package are
 * built with: it sets the hooks of the host's module it serves. Found by
 * name.
 */
interface AsukaPlugin {
    fun register(rt: AsukaRuntime)
}

/** The optional modules' plugins, by class name: platform/push's, platform/periodic's. */
private val plugins = listOf(
    "dev.asuka.push.AsukaPushPlugin",
    "dev.asuka.periodic.AsukaPeriodicPlugin",
)

/**
 * Makes the host's modules, and the optional modules' hooks this app was
 * built with, on the first call of one: an app that calls none makes none
 * (nor looks for the plugins' classes). An app's module of the same name
 * stays the app's. Main thread.
 */
private fun AsukaRuntime.builtins() {
    if (builtinModules) return
    builtinModules = true
    val apps = HashMap(modules)
    registerBuiltinModules()
    modules.putAll(apps)
    for (name in plugins) {
        try {
            (Class.forName(name).getDeclaredConstructor().newInstance() as AsukaPlugin).register(this)
        } catch (_: ReflectiveOperationException) {
        }
    }
}
