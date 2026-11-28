package dev.asuka

import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.widget.RemoteViews
import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID

/**
 * The widget a layout is drawn in: its kind, its id, its configuration
 * (its params' values) and the titles platform/widget's param shows.
 */
class AsukaWidgetScope(
    val kind: String,
    val id: Int,
    val config: Map<String, String> = emptyMap(),
    val titles: Map<String, String> = emptyMap(),
) {
    val configJSON: String get() = JSONObject(config.toSortedMap() as Map<*, *>).toString()

    /**
     * The broadcast a widget's button, toggle or action part sends the
     * host's receiver (AsukaWidgetActionReceiver): a toggle's mutable on
     * Android 12, where the system fills in the state the user set.
     */
    fun action(ctx: Context, action: String, value: String, toggle: Boolean): PendingIntent {
        val i = Intent(ctx, AsukaWidgetActionReceiver::class.java)
            .setData(Uri.Builder().scheme("asuka-widget").authority(kind).appendPath(id.toString()).appendPath(action)
                .appendPath(value).appendQueryParameter("toggle", toggle.toString()).build())
            .putExtra("kind", kind).putExtra("id", id).putExtra("action", action).putExtra("value", value)
            .putExtra("toggle", toggle).putExtra("config", configJSON)
        val mutable = toggle && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (mutable) PendingIntent.FLAG_MUTABLE else PendingIntent.FLAG_IMMUTABLE)
        return PendingIntent.getBroadcast(ctx, 0, i, flags)
    }

    /** A toggle's key: its widget's kind and configuration, its action and value. */
    fun toggleKey(action: String, value: String) = listOf(kind, configJSON, action, value).joinToString("\u0003")
}

/**
 * A widget's button, toggle or action part was tapped: it keeps a
 * toggle's new state (the widget shows it at once), starts the app's code
 * without a screen if it isn't running, and hands it the action
 * (platform/widget's actions()), keeping the receiver until the app is
 * done with it (or [AsukaWidgetActions.SECONDS]).
 */
class AsukaWidgetActionReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        val kind = intent.getStringExtra("kind") ?: return
        val action = intent.getStringExtra("action").orEmpty()
        val value = intent.getStringExtra("value").orEmpty()
        val toggle = intent.getBooleanExtra("toggle", false)
        val config = runCatching { JSONObject(intent.getStringExtra("config") ?: "{}") }.getOrDefault(JSONObject())
        val scope = AsukaWidgetScope(kind, intent.getIntExtra("id", 0), config.keys().asSequence().associateWith { config.optString(it) })
        val token = UUID.randomUUID().toString()
        var isOn = false
        if (toggle) {
            isOn = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S && intent.hasExtra(RemoteViews.EXTRA_CHECKED)) {
                intent.getBooleanExtra(RemoteViews.EXTRA_CHECKED, false)
            } else {
                !AsukaWidgets.isOn(ctx, scope, action, value)
            }
            AsukaWidgets.setToggle(ctx, scope.toggleKey(action, value), kind, isOn, token)
            AsukaWidgets.update(ctx, kind) // the new state, at once
        }
        val o = JSONObject().put("token", token).put("kind", kind).put("id", action).put("value", value).put("isOn", isOn)
            .put("config", config).put("atMs", System.currentTimeMillis())
        val pending = goAsync()
        AsukaWidgetActions.run(ctx, token, kind, o.toString().toByteArray()) { pending.finish() }
    }
}

/**
 * platform/widget's actions on the host's side: "nextAction" gives the
 * app's code each one (sdk/widget's WireAction), "actionDone" (the token)
 * says it's done with one, as "set" of its kind does; "stop" ends a wait.
 */
object AsukaWidgetActions {
    const val SECONDS = 9L
    private val main = Handler(Looper.getMainLooper())
    private val items = ArrayDeque<Triple<String, String, ByteArray>>() // token, kind, the action
    private val waiting = ArrayDeque<(Result<ByteArray>) -> Unit>()
    private val given = HashMap<String, String>() // token: kind, of those the app's code has
    private val pending = HashMap<String, () -> Unit>() // token: the receiver's finish

    /** Hands the action to the app's code, starting it if it hasn't; done once the app is. Any thread. */
    fun run(ctx: Context, token: String, kind: String, action: ByteArray, finish: () -> Unit) = main.post {
        pending[token] = finish
        AsukaRuntime.get(ctx).startHeadless()
        val w = waiting.removeFirstOrNull()
        if (w != null) {
            given[token] = kind
            w(Result.success(action))
        } else items.addLast(Triple(token, kind, action))
        main.postDelayed({ done(ctx, token) }, SECONDS * 1000)
    }

    /** The app is done with the action: its receiver finishes; a toggle's state is the app's again. */
    fun done(ctx: Context, token: String) {
        given.remove(token)
        pending.remove(token)?.invoke()
        if (AsukaWidgets.dropToggles(ctx) { it.optString("token") == token }) AsukaWidgets.update(ctx, null)
    }

    /** The app set the kind's entries: the actions of it the app's code has are done. */
    fun settled(ctx: Context, kind: String) {
        val had = given.filterValues { it == kind }.keys.toSet()
        for (t in had) {
            given.remove(t)
            pending.remove(t)?.invoke()
        }
        AsukaWidgets.dropToggles(ctx) { it.optString("token") in had }
    }

    fun methods(ctx: Context): Map<String, AsukaModuleMethod> = mapOf(
        "nextAction" to { _, reply ->
            val i = items.removeFirstOrNull()
            if (i == null) waiting.addLast(reply)
            else {
                given[i.first] = i.second
                reply(Result.success(i.third))
            }
        },
        "actionDone" to { args, reply ->
            done(ctx, String(args, Charsets.UTF_8))
            reply(Result.success(ByteArray(0)))
        },
        "stop" to { args, reply ->
            if (String(args, Charsets.UTF_8) == "nextAction") {
                val ws = ArrayList(waiting)
                waiting.clear()
                for (w in ws) w(Result.failure(Exception("stopped")))
            }
            reply(Result.success(ByteArray(0)))
        },
    )
}

/**
 * A kind's params (asuka.toml's widgets.params), as the CLI gives its
 * provider's receiver (the dev.asuka.widget.params meta-data): each one's
 * name, title, type, default, options and entity.
 */
class AsukaWidgetParam(val name: String, val title: String, val type: String, val default: String,
                       val prompt: String, val options: List<Pair<String, String>>, val entity: String, val optional: Boolean) {
    companion object {
        fun list(s: String?): List<AsukaWidgetParam> {
            val a = runCatching { JSONArray(s ?: return emptyList()) }.getOrNull() ?: return emptyList()
            return List(a.length()) { i ->
                val o = a.getJSONObject(i)
                val opts = o.optJSONArray("options")
                AsukaWidgetParam(o.optString("name"), o.optString("title"), o.optString("type"), o.optString("default"),
                    o.optString("prompt"), List(opts?.length() ?: 0) { j -> opts!!.getJSONObject(j).let { it.optString("value") to it.optString("title") } },
                    o.optString("entity"), o.optBoolean("optional"))
            }
        }

        /** The params of the provider's kind (its receiver's meta-data). */
        fun of(ctx: Context, provider: ComponentName): List<AsukaWidgetParam> = list(runCatching {
            ctx.packageManager.getReceiverInfo(provider, PackageManager.GET_META_DATA).metaData?.getString("dev.asuka.widget.params")
        }.getOrNull())
    }
}
