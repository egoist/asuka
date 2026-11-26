// platform/document on Android: the files (a path, or a content:// URI the
// system gave the app), read and written on the IO dispatcher.
package platform.document

import android.net.Uri
import dev.asuka.AsukaModules
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

class AppFiles : Files {
    private fun isUri(path: String) = path.startsWith("content://") || path.startsWith("file://")

    override suspend fun read(path: String): ByteArray = withContext(Dispatchers.IO) {
        if (isUri(path)) {
            AsukaModules.context.contentResolver.openInputStream(Uri.parse(path))?.use { it.readBytes() }
                ?: throw java.io.FileNotFoundException(path)
        } else {
            File(path).readBytes()
        }
    }

    override suspend fun write(path: String, data: ByteArray) = withContext(Dispatchers.IO) {
        if (isUri(path)) {
            AsukaModules.context.contentResolver.openOutputStream(Uri.parse(path), "wt")?.use { it.write(data) }
                ?: throw java.io.FileNotFoundException(path)
        } else {
            val f = File(path)
            val tmp = File(f.parentFile, ".${f.name}.tmp")
            tmp.writeBytes(data)
            if (!tmp.renameTo(f)) {
                tmp.delete()
                f.writeBytes(data)
            }
        }
        kotlin.Unit
    }

    override suspend fun modified(path: String): Long = withContext(Dispatchers.IO) {
        if (isUri(path)) 0L else File(path).let { if (it.exists()) it.lastModified() else 0L }
    }

    // A locked file is one that can't be written (a content:// URI's
    // provider decides that itself: never locked here).
    override fun locked(path: String): Boolean = !isUri(path) && File(path).let { it.exists() && !it.canWrite() }

    override fun setLocked(path: String, on: Boolean) {
        if (isUri(path)) throw UnsupportedOperationException("document: a content URI can't be locked")
        if (!File(path).setWritable(!on, false)) throw java.io.IOException("document: can't lock or unlock $path")
    }
}

fun makeFiles(): Files = AppFiles()
