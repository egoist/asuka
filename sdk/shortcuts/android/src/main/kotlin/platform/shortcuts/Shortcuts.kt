// platform/shortcuts on Android: each of the app's things a shortcut of
// asuka.toml offers is a launcher shortcut of its own (a dynamic one,
// opening the shortcut's URL with the thing's id), as many as the
// launcher shows. The shortcuts that take things are the CLI's
// res/values string-array asuka_entity_shortcuts: id|kind|param|title|url.
package platform.shortcuts

import android.content.Intent
import android.content.pm.ShortcutInfo
import android.content.pm.ShortcutManager
import android.net.Uri
import dev.asuka.AsukaModules
import org.json.JSONArray
import java.net.URLEncoder

class LauncherShortcuts : Store {
    override fun set(kind: String, json: String) {
        val app = AsukaModules.context
        val sm = app.getSystemService(ShortcutManager::class.java) ?: return
        val res = app.resources.getIdentifier("asuka_entity_shortcuts", "array", app.packageName)
        val templates = if (res != 0) app.resources.getStringArray(res).map { it.split("|", limit = 5) }.filter { it.size == 5 && it[1] == kind } else emptyList()
        val items = JSONArray(json)
        // This kind's shortcuts go; the others' stay.
        val prefix = templates.map { "${it[0]}:" }
        val kept = sm.dynamicShortcuts.filter { s -> prefix.none { s.id.startsWith(it) } }
        val room = (sm.maxShortcutCountPerActivity - sm.manifestShortcuts.size - kept.size).coerceAtLeast(0)
        val made = ArrayList<ShortcutInfo>()
        for (t in templates) {
            val (id, _, param, _, url) = t
            for (i in 0 until items.length()) {
                if (made.size >= room) break
                val e = items.getJSONObject(i)
                val eid = e.optString("id")
                val title = e.optString("title").ifEmpty { eid }
                if (eid.isEmpty()) continue
                val uri = url.replace("{$param}", URLEncoder.encode(eid, "UTF-8").replace("+", "%20"))
                made.add(
                    ShortcutInfo.Builder(app, "$id:$eid")
                        .setShortLabel(title.take(25))
                        .setLongLabel(title)
                        .setIntent(Intent(Intent.ACTION_VIEW, Uri.parse(uri)).setPackage(app.packageName))
                        .setRank(i)
                        .build()
                )
            }
        }
        sm.dynamicShortcuts = kept + made
    }
}

fun makeStore(): Store = LauncherShortcuts()
