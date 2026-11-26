// platform/car's templates on Android Auto, on this computer's JVM
// (Robolectric, and the Car App Library's test library: a TestCarContext
// stands in for the car): what each of Asuka's templates becomes, what
// choosing does, and how a stack shown changes the car's screens.
package platform.car

import android.content.Intent
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import androidx.car.app.model.GridTemplate
import androidx.car.app.model.ListTemplate
import androidx.car.app.model.MessageTemplate
import androidx.car.app.model.OnClickDelegate
import androidx.car.app.model.PaneTemplate
import androidx.car.app.model.PlaceListMapTemplate
import androidx.car.app.model.Row
import androidx.car.app.model.TabTemplate
import androidx.car.app.model.GridItem
import androidx.car.app.OnDoneCallback
import androidx.car.app.serialization.Bundleable
import androidx.car.app.testing.SessionController
import androidx.car.app.testing.TestCarContext
import androidx.car.app.testing.TestScreenManager
import androidx.lifecycle.Lifecycle
import androidx.test.core.app.ApplicationProvider
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class TemplatesTest {
    private lateinit var car: TestCarContext
    private val chosen = ArrayList<Long>()
    private val backs = ArrayList<Long>()
    private var stop: () -> Unit = {}

    @Before fun setUp() {
        car = TestCarContext.createCarContext(ApplicationProvider.getApplicationContext())
        level(7)
        CarHub.starter = {}
        CarImages.symbols = { _, _ -> ColorDrawable(Color.WHITE) }
        CarHub.show(emptyList())
        stop = CarHub.listen({ chosen.add(it) }, { backs.add(it) })
    }

    @After fun tearDown() {
        stop()
        CarHub.session = null
    }

    /** The car's Car App API level (the host says it as it connects). */
    private fun level(n: Int) {
        car.updateHandshakeInfo(androidx.car.app.HandshakeInfo("com.google.android.projection.gearhead", n))
    }

    @Test fun tabsFromLevel6() {
        val tabs = Shown(id = 1, kind = Kind.TABS, tabs = listOf(
            library.copy(id = 0, text = "Album", symbol = "music.note.list"),
            Shown(kind = Kind.GRID, text = "Covers", sections = listOf(ShownSection(items = listOf(ShownItem(run = 5, title = "Evening"))))),
        ))
        val t = TemplateScreen(car, tabs).onGetTemplate() as TabTemplate
        assertEquals(listOf("Album", "Covers"), t.tabs.map { it.title.toString() })
        assertEquals("tab0", t.activeTabContentId)
        assertTrue(t.tabContents.template is ListTemplate)
        // Before level 6, the first tab alone.
        level(5)
        val first = TemplateScreen(car, tabs).onGetTemplate() as ListTemplate
        @Suppress("DEPRECATION") assertEquals("Library", first.title.toString())
    }

    private fun click(d: OnClickDelegate?) {
        d!!.sendClick(object : OnDoneCallback {
            override fun onSuccess(response: Bundleable?) {}
            override fun onFailure(response: Bundleable) {}
        })
    }

    private val library = Shown(
        id = 1, kind = Kind.LIST, title = "Library",
        sections = listOf(ShownSection(items = listOf(
            ShownItem(run = 1, title = "Spring", detail = "Vivaldi", browsable = true),
            ShownItem(run = 2, title = "Winter", enabled = false),
        ))),
        actions = listOf(ShownAction(run = 3, title = "Shuffle")),
    )

    @Test fun aListsRows() {
        val t = TemplateScreen(car, library).onGetTemplate() as ListTemplate
        @Suppress("DEPRECATION") assertEquals("Library", t.title.toString())
        val rows = t.singleList!!.items.map { it as Row }
        assertEquals(listOf("Spring", "Winter"), rows.map { it.title.toString() })
        assertEquals("Vivaldi", rows[0].texts[0].toString())
        assertTrue(rows[0].isBrowsable)
        click(rows[0].onClickDelegate)
        assertEquals(listOf(1L), chosen)
        assertEquals("Shuffle", t.actionStrip!!.actions[0].title.toString())
        click(t.actionStrip!!.actions[0].onClickDelegate)
        assertEquals(listOf(1L, 3L), chosen)
    }

    @Test fun sectionsHaveHeaders() {
        val s = library.copy(sections = listOf(
            ShownSection(title = "Recent", items = listOf(ShownItem(run = 1, title = "A"))),
            ShownSection(title = "All", items = listOf(ShownItem(run = 2, title = "B"), ShownItem(run = 3, title = "C"))),
        ))
        val t = TemplateScreen(car, s).onGetTemplate() as ListTemplate
        assertEquals(listOf("Recent", "All"), t.sectionedLists.map { it.header.toString() })
        assertEquals(2, t.sectionedLists[1].itemList.items.size)
    }

    @Test fun aGridsCells() {
        val s = Shown(id = 2, kind = Kind.GRID, title = "Covers", sections = listOf(ShownSection(items = listOf(
            ShownItem(run = 4, title = "Morning", symbol = "sun.max"), ShownItem(run = 5, title = "Evening"),
        ))))
        val t = TemplateScreen(car, s).onGetTemplate() as GridTemplate
        val cells = t.singleList!!.items.map { it as GridItem }
        assertEquals(listOf("Morning", "Evening"), cells.map { it.title.toString() })
        click(cells[1].onClickDelegate)
        assertEquals(listOf(5L), chosen)
    }

    @Test fun infoAndMessage() {
        val info = TemplateScreen(car, Shown(id = 2, kind = Kind.INFO, title = "Main St",
            fields = listOf(Field("Free", "2 of 4"), Field("Power", "150 kW")),
            actions = listOf(ShownAction(run = 6, title = "Reserve")))).onGetTemplate() as PaneTemplate
        assertEquals(listOf("Free", "Power"), info.pane.rows.map { it.title.toString() })
        assertEquals("150 kW", info.pane.rows[1].texts[0].toString())
        click(info.pane.actions[0].onClickDelegate)
        assertEquals(listOf(6L), chosen)

        val m = TemplateScreen(car, Shown(id = 3, kind = Kind.MESSAGE, title = "Done", text = "All played", symbol = "checkmark")).onGetTemplate() as MessageTemplate
        assertEquals("All played", m.message.toString())
    }

    @Test fun placesOnAMap() {
        val t = TemplateScreen(car, Shown(id = 2, kind = Kind.PLACES, title = "Chargers", places = listOf(
            ShownPlace(run = 7, title = "Main St", detail = "2 free", latitude = 35.68, longitude = 139.76, distance = 1200.0),
            ShownPlace(title = "Station", latitude = 35.69, longitude = 139.70, distance = -1.0),
        ))).onGetTemplate() as PlaceListMapTemplate
        val row = t.itemList!!.items[0] as Row
        assertEquals("Main St", row.title.toString())
        assertEquals(35.68, row.metadata!!.place!!.location.latitude, 1e-9)
        assertTrue(row.texts[0].toCharSequence().toString().endsWith("2 free"))
        click(row.onClickDelegate)
        assertEquals(listOf(7L), chosen)
        // No distance: a row that shows more, which Android Auto lists without one.
        assertTrue((t.itemList!!.items[1] as Row).isBrowsable)
    }

    @Test fun theStackFollowsAsuka() {
        val session = AsukaCarSession()
        val controller = SessionController(session, car, Intent())
        controller.moveToState(Lifecycle.State.RESUMED)
        val sm = car.getCarService(TestScreenManager::class.java)
        assertTrue(CarHub.connected)
        // Asuka's stack: the root, then a template pushed.
        CarHub.show(listOf(library))
        val album = Shown(id = 2, kind = Kind.LIST, title = "Spring", sections = listOf(ShownSection(items = listOf(ShownItem(run = 8, title = "Allegro")))))
        CarHub.show(listOf(library, album))
        assertEquals(listOf("Library", "Spring"), sm.screensPushed.map { (it as TemplateScreen).shown.title })
        // Changed in place: no new screen.
        CarHub.show(listOf(library, album.copy(sections = listOf(ShownSection(items = listOf(ShownItem(run = 9, title = "Largo")))))))
        assertEquals(2, sm.screensPushed.size)
        val top = sm.screensPushed.last() as TemplateScreen
        val rows = (top.onGetTemplate() as ListTemplate).singleList!!.items.map { (it as Row).title.toString() }
        assertEquals(listOf("Largo"), rows)
        // The car's back button pops it (shown on the car: started), and Asuka hears one is left.
        androidx.car.app.testing.ScreenController(top).moveToState(Lifecycle.State.RESUMED)
        car.onBackPressedDispatcher.onBackPressed()
        assertEquals(listOf(1L), backs)
        // Asuka shows the root alone: nothing more to pop.
        CarHub.show(listOf(library))
        assertEquals(listOf(1L), backs)
        controller.moveToState(Lifecycle.State.DESTROYED)
        assertFalse(CarHub.connected)
        assertNull(CarHub.session)
    }
}
