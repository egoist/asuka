// platform/car for an audio app: Android Auto's media browser over Asuka's
// templates, on this computer's JVM (Robolectric): the tree's root, its
// tabs, a browsable row's children (what choosing it pushes), playing a
// row, and the service a browser connects to.
package platform.car

import android.media.browse.MediaBrowser
import android.os.Looper
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf

@RunWith(RobolectricTestRunner::class)
class MediaTest {
    private val chosen = ArrayList<Long>()
    private val backs = ArrayList<Long>()
    private var stop: () -> Unit = {}
    private val tree = MediaTree("dev.asuka.car")

    private val album = Shown(id = 1, kind = Kind.LIST, title = "A Day in Sines", sections = listOf(ShownSection(items = listOf(
        ShownItem(run = 1, title = "Morning", detail = "Asuka Ensemble", image = "covers/morning.jpg"),
        ShownItem(run = 2, title = "More", browsable = true),
    ))))

    @Before fun setUp() {
        CarHub.starter = {}
        CarHub.show(listOf(album))
        stop = CarHub.listen({ chosen.add(it) }, { backs.add(it) })
    }

    @After fun tearDown() {
        stop()
        CarHub.media = false
    }

    /** What load answers, null while it waits. */
    private class Answer {
        var items: List<MediaBrowser.MediaItem>? = null
    }

    private fun load(id: String): Answer {
        val a = Answer()
        tree.load(id) { a.items = it }
        return a
    }

    private val root = "asuka.root"

    @Test fun theRootIsTheRootsRows() {
        val items = load(root).items!!
        assertEquals(listOf("Morning", "More"), items.map { it.description.title.toString() })
        assertEquals("Asuka Ensemble", items[0].description.subtitle.toString())
        assertTrue(items[0].isPlayable)
        assertTrue(items[1].isBrowsable)
        // The image, through the package's provider.
        assertEquals("content", items[0].description.iconUri!!.scheme)
        assertEquals("dev.asuka.car.asuka.car", items[0].description.iconUri!!.authority)
        assertEquals("covers/morning.jpg", items[0].description.iconUri!!.getQueryParameter("s"))
        // Playing a row runs its handler.
        tree.play(items[0].mediaId!!)
        assertEquals(listOf(1L), chosen)
    }

    @Test fun beforeTheAppShowsTheRootWaits() {
        CarHub.show(emptyList())
        val a = load(root)
        assertNull(a.items)
        CarHub.show(listOf(album))
        assertEquals(2, a.items!!.size)
    }

    @Test fun tabsAreTheRootsChildren() {
        CarHub.show(listOf(Shown(id = 1, kind = Kind.TABS, tabs = listOf(
            album.copy(id = 0, text = "Album"),
            Shown(kind = Kind.GRID, text = "Covers", sections = listOf(ShownSection(items = listOf(ShownItem(run = 3, title = "Evening"))))),
        ))))
        val tabs = load(root).items!!
        assertEquals(listOf("Album", "Covers"), tabs.map { it.description.title.toString() })
        assertTrue(tabs.all { it.isBrowsable })
        val covers = load(tabs[1].mediaId!!).items!!
        assertEquals(listOf("Evening"), covers.map { it.description.title.toString() })
        tree.play(covers[0].mediaId!!)
        assertEquals(listOf(3L), chosen)
    }

    @Test fun aBrowsableRowsChildrenAreWhatItPushes() {
        val more = load(root).items!![1]
        val a = load(more.mediaId!!)
        // Chosen: Asuka runs its handler, which pushes.
        assertEquals(listOf(2L), chosen)
        assertNull(a.items)
        CarHub.show(listOf(album, Shown(id = 2, kind = Kind.LIST, title = "More", sections = listOf(ShownSection(items = listOf(ShownItem(run = 4, title = "Night")))))))
        assertEquals(listOf("Night"), a.items!!.map { it.description.title.toString() })
        // Browsing it again from the root: back to the root first, then chosen again.
        val again = load(more.mediaId!!)
        assertEquals(listOf(1L), backs)
        assertEquals(listOf(2L, 2L), chosen)
        CarHub.show(listOf(album, Shown(id = 3, kind = Kind.LIST, title = "More", sections = listOf(ShownSection(items = listOf(ShownItem(run = 5, title = "Dawn")))))))
        assertEquals(listOf("Dawn"), again.items!!.map { it.description.title.toString() })
    }

    @Test fun aHandlerThatPushesNothingAnswersEmpty() {
        val a = load(load(root).items!![1].mediaId!!)
        assertNull(a.items)
        shadowOf(Looper.getMainLooper()).idleFor(java.time.Duration.ofSeconds(6))
        assertEquals(0, a.items!!.size)
    }

    @Test fun androidAutoConnects() {
        val c = Robolectric.buildService(AsukaCarMediaService::class.java).create()
        val s = c.get()
        assertFalse(CarHub.connected)
        val r = s.onGetRoot("com.google.android.projection.gearhead", 0, null)
        assertEquals("asuka.root", r.rootId)
        assertTrue(CarHub.connected)
        c.destroy()
        assertFalse(CarHub.connected)
    }
}
