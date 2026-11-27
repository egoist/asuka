// platform/car's templates on Android Auto: the Car App Library's
// CarAppService, its Session, and a Screen per template of Asuka's stack.
// A stack shown changes the screens on the car: the ones it has keep
// (their templates made again, which the car takes as a refresh), the
// ones gone are popped, the new ones pushed. The car's back button pops
// one, and Asuka hears how many are left. A navigation app's root is its
// map, drawn on the car's surface (Navigation.kt, CarMap.kt).
package platform.car

import android.content.Intent
import android.content.pm.ApplicationInfo
import android.text.SpannableString
import android.text.Spanned
import android.text.TextUtils
import androidx.activity.OnBackPressedCallback
import androidx.car.app.CarAppService
import androidx.car.app.CarContext
import androidx.car.app.Screen
import androidx.car.app.Session
import androidx.car.app.constraints.ConstraintManager
import androidx.car.app.model.Action
import androidx.car.app.model.ActionStrip
import androidx.car.app.model.CarColor
import androidx.car.app.model.CarIcon
import androidx.car.app.model.CarLocation
import androidx.car.app.model.Distance
import androidx.car.app.model.DistanceSpan
import androidx.car.app.model.GridItem
import androidx.car.app.model.GridTemplate
import androidx.car.app.model.ItemList
import androidx.car.app.model.ListTemplate
import androidx.car.app.model.MessageTemplate
import androidx.car.app.model.Metadata
import androidx.car.app.model.Pane
import androidx.car.app.model.PaneTemplate
import androidx.car.app.model.Place
import androidx.car.app.model.PlaceListMapTemplate
import androidx.car.app.model.PlaceMarker
import androidx.car.app.model.Row
import androidx.car.app.model.SectionedItemList
import androidx.car.app.model.Tab
import androidx.car.app.model.TabContents
import androidx.car.app.model.TabTemplate
import androidx.car.app.model.Template
import androidx.car.app.validation.HostValidator
import androidx.car.app.versioning.CarAppApiLevels
import androidx.core.graphics.drawable.IconCompat

/** The service Android Auto binds, with the category the CLI gives it. */
class AsukaCarService : CarAppService() {
    override fun createHostValidator(): HostValidator =
        // A debug build takes any host (the Desktop Head Unit, a test's);
        // a release only the car hosts Google lists.
        if (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0) HostValidator.ALLOW_ALL_HOSTS_VALIDATOR
        else HostValidator.Builder(applicationContext).addAllowedHosts(androidx.car.app.R.array.hosts_allowlist_sample).build()

    override fun onCreateSession(): Session = AsukaCarSession()
}

/** A car showing the app: its screens follow Asuka's stack. */
class AsukaCarSession : Session() {
    private val screens = ArrayList<TemplateScreen>()

    /** A navigation app's map, trip and alerts, once its root is .navigation. */
    internal var navigator: CarNavigator? = null
        private set

    override fun onCreateScreen(intent: Intent): Screen {
        CarHub.start(carContext)
        lifecycle.addObserver(object : androidx.lifecycle.DefaultLifecycleObserver {
            override fun onDestroy(owner: androidx.lifecycle.LifecycleOwner) {
                navigator?.end()
                if (CarHub.session === this@AsukaCarSession) CarHub.session = null
            }
        })
        // The car's back button: a pushed template goes, and Asuka hears it.
        carContext.onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (screens.size > 1) {
                    screens.removeAt(screens.size - 1)
                    carContext.getCarService(androidx.car.app.ScreenManager::class.java).pop()
                    CarHub.back(screens.size)
                }
            }
        })
        val root = TemplateScreen(carContext, CarHub.stack.firstOrNull() ?: placeholder())
        screens.clear()
        screens.add(root)
        CarHub.session = this
        // What else is on the stack goes over the root once it's shown.
        root.lifecycle.addObserver(object : androidx.lifecycle.DefaultLifecycleObserver {
            override fun onStart(owner: androidx.lifecycle.LifecycleOwner) {
                root.lifecycle.removeObserver(this)
                render()
            }
        })
        return root
    }

    private fun placeholder() = Shown(id = 1, kind = Kind.LIST, title = carContext.applicationInfo.loadLabel(carContext.packageManager).toString())

    /** Shows an alert over the map (none: it's gone at once). */
    internal fun alert(a: ShownAlert) {
        val n = navigator
        if (n != null) n.present(a) else CarHub.answered(a.id, -1)
    }

    internal fun dismiss(id: Long) {
        navigator?.dismiss(id)
    }

    /** Shows Asuka's stack: the screens kept are made again, the ones gone popped, the new ones pushed. */
    fun render() {
        val sm = carContext.getCarService(androidx.car.app.ScreenManager::class.java)
        if (screens.isEmpty()) return
        val want = CarHub.stack.ifEmpty { listOf(placeholder()) }
        val was = navigator
        if (want[0].kind == Kind.NAVIGATION) {
            val n = navigator ?: CarNavigator(carContext).also {
                navigator = it
                // The driver moved the map, or came back: the recenter action follows.
                it.map.moved = { screens.firstOrNull()?.invalidate() }
            }
            n.follow(want[0])
            if (n !== was) screens[0].invalidate() // its template now has the trip

        }
        screens[0].update(want[0])
        var i = 1
        while (i < screens.size && i < want.size && screens[i].shown.id == want[i].id && screens[i].shown.kind == want[i].kind) {
            screens[i].update(want[i])
            i++
        }
        if (i < screens.size) {
            val to = screens[i - 1]
            while (screens.size > i) screens.removeAt(screens.size - 1)
            sm.popTo(to.marker ?: "")
        }
        while (i < want.size) {
            val s = TemplateScreen(carContext, want[i])
            s.marker = "asuka.${want[i].id}"
            screens.add(s)
            sm.push(s)
            i++
        }
    }
}

/** A screen showing one of Asuka's templates. */
class TemplateScreen(context: CarContext, shown: Shown) : Screen(context) {
    var shown: Shown = shown
        private set

    init {
        if (shown.id == 1L) marker = "asuka.root"
    }

    fun update(s: Shown) {
        if (s == shown) return
        shown = s
        invalidate()
    }

    private val root get() = shown.id == 1L

    private val listLimit: Int
        get() = if (carContext.carAppApiLevel >= CarAppApiLevels.LEVEL_2) {
            carContext.getCarService(ConstraintManager::class.java).getContentLimit(ConstraintManager.CONTENT_LIMIT_TYPE_LIST)
        } else 6

    private val gridLimit: Int
        get() = if (carContext.carAppApiLevel >= CarAppApiLevels.LEVEL_2) {
            carContext.getCarService(ConstraintManager::class.java).getContentLimit(ConstraintManager.CONTENT_LIMIT_TYPE_GRID)
        } else 6

    private val headerAction get() = if (root) Action.APP_ICON else Action.BACK

    override fun onGetTemplate(): Template = template(shown, inTab = false)

    @Suppress("DEPRECATION")
    private fun template(s: Shown, inTab: Boolean): Template = when (s.kind) {
        Kind.GRID -> GridTemplate.Builder().apply {
            if (!inTab) {
                setTitle(s.title)
                setHeaderAction(headerAction)
                strip(s.actions)?.let { setActionStrip(it) }
            }
            val items = s.sections.flatMap { it.items }.take(gridLimit)
            val list = ItemList.Builder()
            for (i in items) list.addItem(gridItem(i))
            if (items.isEmpty()) setLoading(false)
            setSingleList(list.build())
        }.build()
        Kind.TABS -> tabs(s)
        Kind.INFO -> PaneTemplate.Builder(Pane.Builder().apply {
            for (f in s.fields.take(4)) addRow(Row.Builder().setTitle(f.title).apply { if (f.detail.isNotEmpty()) addText(f.detail) }.build())
            if (s.fields.isEmpty()) addRow(Row.Builder().setTitle(s.title.ifEmpty { " " }).build())
            for (a in s.actions.take(2)) addAction(action(a))
        }.build()).apply {
            if (!inTab) {
                setTitle(s.title)
                setHeaderAction(headerAction)
            }
        }.build()
        Kind.MESSAGE -> MessageTemplate.Builder(s.text.ifEmpty { s.title }).apply {
            if (!inTab) {
                setTitle(s.title)
                setHeaderAction(headerAction)
            }
            CarImages.symbol(carContext, s.symbol)?.let { setIcon(icon(it, tint = true)) }
            for (a in s.actions.take(2)) addAction(action(a))
        }.build()
        Kind.PLACES -> if (inTab) list(s.copy(sections = listOf(ShownSection(items = s.places.map { ShownItem(run = it.run, title = it.title, detail = it.detail) }))), true) else places(s)
        Kind.NAVIGATION -> if (inTab) list(s, true) else navigationTemplate(s, if (root) CarHub.session?.navigator else null, root)
        Kind.NOW_PLAYING -> MessageTemplate.Builder(carContext.applicationInfo.loadLabel(carContext.packageManager)).setHeaderAction(Action.BACK).build()
        else -> list(s, inTab)
    }

    @Suppress("DEPRECATION")
    private fun list(s: Shown, inTab: Boolean): ListTemplate = ListTemplate.Builder().apply {
        if (!inTab) {
            setTitle(s.title)
            setHeaderAction(headerAction)
            strip(s.actions)?.let { setActionStrip(it) }
        }
        var left = listLimit
        val sections = s.sections.filter { it.items.isNotEmpty() }
        if (sections.size <= 1 && (sections.firstOrNull()?.title ?: "").isEmpty()) {
            val list = ItemList.Builder()
            for (i in sections.firstOrNull()?.items.orEmpty().take(left)) list.addItem(row(i))
            if (s.empty.isNotEmpty()) list.setNoItemsMessage(s.empty)
            setSingleList(list.build())
        } else {
            for (sec in sections) {
                if (left <= 0) break
                val list = ItemList.Builder()
                for (i in sec.items.take(left)) list.addItem(row(i))
                left -= sec.items.size
                addSectionedList(SectionedItemList.create(list.build(), sec.title.ifEmpty { " " }))
            }
        }
    }.build()

    @Suppress("DEPRECATION")
    private fun places(s: Shown): PlaceListMapTemplate = PlaceListMapTemplate.Builder().apply {
        setTitle(s.title)
        setHeaderAction(headerAction)
        val list = ItemList.Builder()
        for ((n, p) in s.places.take(listLimit).withIndex()) {
            val place = Place.Builder(CarLocation.create(p.latitude, p.longitude))
                .setMarker(PlaceMarker.Builder().setLabel(((n % 26) + 'A'.code).toChar().toString()).build()).build()
            list.addItem(Row.Builder().setTitle(p.title).apply {
                // A place's row says its distance, unless it pushes more (a browsable one).
                if (p.distance >= 0) {
                    val text = SpannableString("  ").apply { setSpan(DistanceSpan.create(distance(p.distance)), 0, 1, Spanned.SPAN_INCLUSIVE_INCLUSIVE) }
                    addText(if (p.detail.isNotEmpty()) SpannableString(TextUtils.concat(text, " · ", p.detail)) else text)
                } else if (p.detail.isNotEmpty()) {
                    addText(p.detail)
                }
                val run = p.run
                if (run > 0 || p.distance < 0) {
                    setOnClickListener { CarHub.chose(run) }
                    if (p.distance < 0) setBrowsable(true)
                }
            }.setMetadata(Metadata.Builder().setPlace(place).build()).build())
        }
        setItemList(list.build())
    }.build()

    private fun distance(meters: Double): Distance = carDistance(carContext, meters)

    private fun tabs(s: Shown): Template {
        val tabs = s.tabs
        if (tabs.isEmpty()) return list(s, false)
        // Tabs are car API level 6's: before, the first tab alone.
        if (carContext.carAppApiLevel < CarAppApiLevels.LEVEL_6) return template(tabs[0].copy(id = shown.id), false)
        val active = selected.coerceIn(0, tabs.size - 1)
        return TabTemplate.Builder(object : TabTemplate.TabCallback {
            override fun onTabSelected(id: String) {
                selected = id.removePrefix("tab").toIntOrNull() ?: 0
                invalidate()
            }
        }).apply {
            setHeaderAction(Action.APP_ICON)
            for ((n, t) in tabs.withIndex()) {
                val sym = CarImages.symbol(carContext, t.symbol.ifEmpty { "circle" }) ?: CarImages.symbol(carContext, "star")
                addTab(Tab.Builder().setTitle(t.text.ifEmpty { t.title }).setContentId("tab$n").apply {
                    setIcon(if (sym != null) icon(sym, tint = true) else CarIcon.APP_ICON)
                }.build())
            }
            setTabContents(TabContents.Builder(template(tabs[active], inTab = true)).build())
            setActiveTabContentId("tab$active")
        }.build()
    }

    private var selected = 0

    private fun strip(actions: List<ShownAction>): ActionStrip? {
        if (actions.isEmpty()) return null
        val b = ActionStrip.Builder()
        for (a in actions.take(2)) b.addAction(action(a))
        return b.build()
    }

    internal fun action(a: ShownAction): Action = Action.Builder().apply {
        val sym = if (a.symbol.isNotEmpty()) CarImages.symbol(carContext, a.symbol) else null
        if (a.title.isNotEmpty() || sym == null) setTitle(a.title.ifEmpty { " " })
        if (sym != null) setIcon(icon(sym, tint = true))
        if (a.style == 2L) setBackgroundColor(CarColor.RED)
        if (carContext.carAppApiLevel >= CarAppApiLevels.LEVEL_5) setEnabled(a.enabled && a.run > 0)
        val run = a.run
        setOnClickListener { CarHub.chose(run) }
    }.build()

    private fun image(i: ShownItem): CarIcon? {
        CarImages.get(carContext, i.image) { invalidate() }?.let { return icon(it, tint = false) }
        return CarImages.symbol(carContext, i.symbol)?.let { icon(it, tint = true) }
    }

    private fun row(i: ShownItem): Row = Row.Builder().apply {
        setTitle(i.title)
        if (i.detail.isNotEmpty()) addText(i.detail)
        image(i)?.let { setImage(it, if (i.image.isNotEmpty()) Row.IMAGE_TYPE_LARGE else Row.IMAGE_TYPE_ICON) }
        val run = i.run
        if (run > 0) {
            setOnClickListener { CarHub.chose(run) }
            if (i.browsable) setBrowsable(true)
        }
        if (carContext.carAppApiLevel >= CarAppApiLevels.LEVEL_5) setEnabled(i.enabled)
    }.build()

    private fun gridItem(i: ShownItem): GridItem = GridItem.Builder().apply {
        setTitle(i.title)
        if (i.detail.isNotEmpty()) setText(i.detail)
        setImage(image(i) ?: CarIcon.APP_ICON, GridItem.IMAGE_TYPE_LARGE)
        val run = i.run
        if (run > 0) setOnClickListener { CarHub.chose(run) }
    }.build()

    private fun icon(b: android.graphics.Bitmap, tint: Boolean): CarIcon =
        CarIcon.Builder(IconCompat.createWithBitmap(b)).apply { if (tint) setTint(CarColor.DEFAULT) }.build()
}
