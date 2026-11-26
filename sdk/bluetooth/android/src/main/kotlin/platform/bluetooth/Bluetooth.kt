// platform/bluetooth on Android: the BluetoothLeScanner, a BluetoothGatt
// per connection (its operations one at a time, as Android wants them:
// each waits for its callback), a BluetoothGattServer and the
// BluetoothLeAdvertiser for the app as a peripheral. Android 12's
// nearby devices permissions are asked for as the app first needs them.
// The callbacks come on binder threads: everything goes to the main
// looper. A background scan is one the system runs for the app, reporting
// to BluetoothScanReceiver (BackgroundScan), so it outlives the process.
package platform.bluetooth

import android.Manifest
import android.annotation.SuppressLint
import android.app.PendingIntent
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.os.SystemClock
import android.util.Base64
import androidx.activity.ComponentActivity
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.RequiresApi
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import dev.asuka.AsukaModules
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.UUID
import kotlin.concurrent.thread
import kotlin.coroutines.resume

private val main = Handler(Looper.getMainLooper())
private const val BASE = "-0000-1000-8000-00805f9b34fb"
private val CCCD: UUID = UUID.fromString("00002902$BASE")

/** A UUID as the package writes it: lowercase, a SIG one in four digits. */
private fun short(u: UUID): String {
    val s = u.toString().lowercase()
    return if (s.startsWith("0000") && s.endsWith(BASE)) s.substring(4, 8) else s
}

private fun full(u: String): UUID = UUID.fromString(if (u.length == 4) "0000$u$BASE" else u)

@SuppressLint("MissingPermission") // asked for first (permitted)
class AndroidAdapter(private val ctx: Context) : Adapter {
    private val manager get() = ctx.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager?
    private val adapter: BluetoothAdapter? get() = manager?.adapter

    private fun granted(p: String) = ctx.checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED
    // Before Android 12 there are no nearby devices permissions: a scan
    // needs the location permission, and the rest the install's.
    private val central = if (Build.VERSION.SDK_INT >= 31) arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
    private val peripheral = if (Build.VERSION.SDK_INT >= 31) arrayOf(Manifest.permission.BLUETOOTH_ADVERTISE, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf()

    override fun state(): State {
        val a = adapter ?: return State.UNSUPPORTED
        if (Build.VERSION.SDK_INT >= 31 && !granted(Manifest.permission.BLUETOOTH_CONNECT)) return State.UNAUTHORIZED
        return when (a.state) {
            BluetoothAdapter.STATE_ON -> State.ON
            BluetoothAdapter.STATE_TURNING_ON, BluetoothAdapter.STATE_TURNING_OFF -> State.RESETTING
            else -> State.OFF
        }
    }

    override suspend fun watchState(changed: (State) -> Unit) {
        val r = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) { changed(state()) }
        }
        withContext(Dispatchers.Main) {
            changed(state())
            ContextCompat.registerReceiver(ctx, r, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED), ContextCompat.RECEIVER_NOT_EXPORTED)
        }
        try {
            awaitCancellation()
        } finally {
            main.post { runCatching { ctx.unregisterReceiver(r) } }
        }
    }

    /** Asks for the permissions not granted yet. */
    private suspend fun permitted(perms: Array<String>): Boolean = withContext(Dispatchers.Main) {
        if (perms.all { granted(it) }) return@withContext true
        val act = AsukaModules.activity as? ComponentActivity ?: return@withContext false
        suspendCancellableCoroutine { c ->
            var l: ActivityResultLauncher<Array<String>>? = null
            l = act.activityResultRegistry.register("asuka-bluetooth-${asks++}", ActivityResultContracts.RequestMultiplePermissions()) { r ->
                l?.unregister()
                if (c.isActive) c.resume(r.values.all { it })
            }
            l.launch(perms)
        }
    }

    override suspend fun request(): State {
        permitted(central)
        return state()
    }

    private suspend fun ready(perms: Array<String>) {
        if (adapter == null) throw BluetoothException(BluetoothError.UNSUPPORTED)
        if (!permitted(perms)) throw BluetoothException(BluetoothError.DENIED)
        if (adapter?.isEnabled != true) throw BluetoothException(BluetoothError.OFF)
    }

    override suspend fun scan(options: ScanOptions, found: (Heard) -> Unit) {
        ready(central)
        val scanner = adapter?.bluetoothLeScanner ?: throw BluetoothException(BluetoothError.OFF)
        if (options.background) return backgroundScan(scanner, options, found)
        val seen = HashSet<String>()
        val cb = object : ScanCallback() {
            override fun onScanResult(type: Int, r: ScanResult) {
                main.post {
                    if (!options.duplicates && !seen.add(r.device.address)) return@post
                    found(advertisement(ctx, r))
                }
            }
        }
        val filters = options.services.map { ScanFilter.Builder().setServiceUuid(ParcelUuid(full(it))).build() }
        withContext(Dispatchers.Main) {
            scanner.startScan(filters.ifEmpty { null }, ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), cb)
        }
        try {
            awaitCancellation()
        } finally {
            main.post { runCatching { scanner.stopScan(cb) } }
        }
    }

    /** A background scan: BackgroundScan's, read until the task is cancelled. */
    private suspend fun backgroundScan(scanner: BluetoothLeScanner, options: ScanOptions, found: (Heard) -> Unit) {
        // Before Android 8 there is no scan the system runs for the app.
        if (Build.VERSION.SDK_INT < 26) throw BluetoothException(BluetoothError.NOT_AVAILABLE)
        // Its receiver is only in the apps that call backgroundScan.
        if (!BackgroundScan.declared(ctx)) throw BluetoothException(BluetoothError.NOT_AVAILABLE)
        val r = BackgroundScan.Reader(options.services, options.duplicates, found)
        val id = withContext(Dispatchers.Main) {
            try {
                BackgroundScan.add(ctx, scanner, r)
            } catch (e: SecurityException) {
                throw BluetoothException(BluetoothError.DENIED)
            }
        }
        try {
            r.failed.await()
        } finally {
            main.post { BackgroundScan.remove(ctx, id) }
        }
    }

    override suspend fun watchRestored(restored: (Brought) -> Unit) {
        // What comes now goes to restored; what the receiver kept before,
        // then.
        val id = withContext(Dispatchers.Main) { BackgroundScan.listenRestored(restored) }
        try {
            val kept = withContext(Dispatchers.IO) { BackgroundScan.takeKept(ctx) }
            if (kept.isNotEmpty()) withContext(Dispatchers.Main) { restored(Brought(advertisements = kept)) }
            awaitCancellation()
        } finally {
            main.post { BackgroundScan.unlistenRestored(id) }
        }
    }

    override fun stopBackgroundScan() {
        main.post { BackgroundScan.stopLeft(ctx) }
    }

    override suspend fun choose(filters: List<Filter>, optionalServices: List<String>): Device = throw BluetoothException(BluetoothError.NOT_AVAILABLE)

    /** A connection: its GATT, and its operations, one at a time. */
    private inner class Conn(val changed: (Link) -> Unit) {
        var gatt: BluetoothGatt? = null
        val ops = Mutex()
        var op: CompletableDeferred<Any?>? = null
        var discovered = false
        val subs = HashMap<String, MutableList<(ByteArray) -> Unit>>()
        val ended = CompletableDeferred<Unit>()
        var mtu = 23

        /** Runs start, then waits for its callback (finish). */
        suspend fun <T> run(start: (BluetoothGatt) -> Boolean): T = ops.withLock {
            val g = gatt ?: throw BluetoothException(BluetoothError.DISCONNECTED)
            val d = CompletableDeferred<Any?>()
            op = d
            val ok = withContext(Dispatchers.Main) { start(g) }
            if (!ok) {
                op = null
                throw BluetoothException(BluetoothError.UNSUPPORTED)
            }
            @Suppress("UNCHECKED_CAST")
            d.await() as T
        }

        fun finish(v: Any?) {
            val d = op
            op = null
            if (v is Throwable) d?.completeExceptionally(v) else d?.complete(v)
        }
    }

    private val conns = HashMap<Long, Conn>()
    private var handles = 0L

    private fun conn(h: Long): Conn = conns[h] ?: throw BluetoothException(BluetoothError.DISCONNECTED)

    override suspend fun connect(device: Device, options: ConnectOptions, changed: (Link) -> Unit) {
        ready(central)
        val dev = runCatching { adapter!!.getRemoteDevice(device.id) }.getOrNull() ?: throw BluetoothException(BluetoothError.NOT_FOUND)
        val h = ++handles
        val c = Conn(changed)
        conns[h] = c
        val cb = object : BluetoothGattCallback() {
            override fun onConnectionStateChange(g: BluetoothGatt, status: Int, state: Int) {
                main.post {
                    when (state) {
                        BluetoothProfile.STATE_CONNECTED -> {
                            changed(Link(state = ConnectionState.CONNECTED, handle = h))
                        }
                        BluetoothProfile.STATE_DISCONNECTED -> {
                            c.finish(BluetoothException(BluetoothError.DISCONNECTED))
                            changed(Link(state = ConnectionState.DISCONNECTED,
                                error = if (status == BluetoothGatt.GATT_SUCCESS) "bluetooth: disconnected" else "bluetooth: disconnected: status $status"))
                            if (!options.autoConnect) c.ended.complete(Unit)
                        }
                    }
                }
            }
            override fun onServicesDiscovered(g: BluetoothGatt, status: Int) = main.post { c.finish(status) }.let {}
            override fun onCharacteristicRead(g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray, status: Int) {
                main.post { c.finish(if (status == BluetoothGatt.GATT_SUCCESS) value else BluetoothException(BluetoothError.UNSUPPORTED)) }
            }
            // Before Android 13 the value is the characteristic's.
            @Suppress("OVERRIDE_DEPRECATION")
            override fun onCharacteristicRead(g: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int) {
                @Suppress("DEPRECATION") val value = ch.value ?: ByteArray(0)
                onCharacteristicRead(g, ch, value, status)
            }
            override fun onCharacteristicWrite(g: BluetoothGatt, ch: BluetoothGattCharacteristic, status: Int) {
                main.post { c.finish(if (status == BluetoothGatt.GATT_SUCCESS) null else BluetoothException(BluetoothError.UNSUPPORTED)) }
            }
            override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
                main.post { c.finish(status) }
            }
            override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray) {
                val key = "${short(ch.service.uuid)} ${short(ch.uuid)}"
                main.post { c.subs[key]?.toList()?.forEach { it(value) } }
            }
            @Suppress("OVERRIDE_DEPRECATION")
            override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
                @Suppress("DEPRECATION") val value = ch.value ?: ByteArray(0)
                onCharacteristicChanged(g, ch, value)
            }
            override fun onReadRemoteRssi(g: BluetoothGatt, rssi: Int, status: Int) = main.post { c.finish(rssi.toLong()) }.let {}
            override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) = main.post { c.mtu = mtu; c.finish(mtu.toLong()) }.let {}
        }
        withContext(Dispatchers.Main) {
            changed(Link(state = ConnectionState.CONNECTING))
            c.gatt = dev.connectGatt(ctx, options.autoConnect, cb, BluetoothDevice.TRANSPORT_LE)
        }
        try {
            val t = options.timeout.inWholeMilliseconds
            if (t > 0) {
                val connected = withTimeoutOrNull(t) {
                    while (c.gatt?.let { manager?.getConnectionState(dev, BluetoothProfile.GATT) } != BluetoothProfile.STATE_CONNECTED) {
                        kotlinx.coroutines.delay(100)
                        if (c.ended.isCompleted) break
                    }
                }
                if (connected == null) {
                    changed(Link(state = ConnectionState.DISCONNECTED, error = "bluetooth: timeout"))
                    return
                }
            }
            c.ended.await()
        } finally {
            conns.remove(h)
            main.post { runCatching { c.gatt?.disconnect(); c.gatt?.close() } }
        }
    }

    private suspend fun discover(c: Conn) {
        if (c.discovered) return
        c.run<Any?> { it.discoverServices() }
        c.discovered = true
    }

    override suspend fun services(connection: Long): List<FoundService> {
        val c = conn(connection)
        discover(c)
        return c.gatt!!.services.map { s ->
            FoundService(uuid = short(s.uuid), characteristics = s.characteristics.map { ch ->
                FoundCharacteristic(uuid = short(ch.uuid), properties = properties(ch.properties), descriptors = ch.descriptors.map { short(it.uuid) })
            })
        }
    }

    private fun properties(p: Int): List<Property> {
        val out = ArrayList<Property>()
        if (p and BluetoothGattCharacteristic.PROPERTY_BROADCAST != 0) out.add(Property.BROADCAST)
        if (p and BluetoothGattCharacteristic.PROPERTY_READ != 0) out.add(Property.READ)
        if (p and BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE != 0) out.add(Property.WRITE_WITHOUT_RESPONSE)
        if (p and BluetoothGattCharacteristic.PROPERTY_WRITE != 0) out.add(Property.WRITE)
        if (p and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0) out.add(Property.NOTIFY)
        if (p and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0) out.add(Property.INDICATE)
        if (p and BluetoothGattCharacteristic.PROPERTY_SIGNED_WRITE != 0) out.add(Property.AUTHENTICATED_SIGNED_WRITES)
        if (p and BluetoothGattCharacteristic.PROPERTY_EXTENDED_PROPS != 0) out.add(Property.EXTENDED_PROPERTIES)
        return out
    }

    private suspend fun characteristic(c: Conn, service: String, char: String): BluetoothGattCharacteristic {
        discover(c)
        return c.gatt?.getService(full(service))?.getCharacteristic(full(char)) ?: throw BluetoothException(BluetoothError.NOT_FOUND)
    }

    override suspend fun mtu(connection: Long): Long {
        val c = conn(connection)
        if (c.mtu == 23) runCatching { c.run<Long> { it.requestMtu(517) } }
        return c.mtu.toLong()
    }

    override suspend fun rssi(connection: Long): Long = conn(connection).run { it.readRemoteRssi() }

    override suspend fun read(connection: Long, service: String, characteristic: String): ByteArray {
        val c = conn(connection)
        val ch = characteristic(c, service, characteristic)
        return c.run { it.readCharacteristic(ch) }
    }

    override suspend fun write(connection: Long, service: String, characteristic: String, value: ByteArray, response: Boolean) {
        val c = conn(connection)
        val ch = characteristic(c, service, characteristic)
        val type = if (response) BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT else BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        if (response) {
            c.run<Any?> { writeCharacteristic(it, ch, value, type) }
        } else {
            c.ops.withLock { withContext(Dispatchers.Main) { c.gatt?.let { writeCharacteristic(it, ch, value, type) } } }
        }
    }

    override suspend fun subscribe(connection: Long, service: String, characteristic: String, value: (ByteArray) -> Unit) {
        val c = conn(connection)
        val ch = characteristic(c, service, characteristic)
        val key = "${short(ch.service.uuid)} ${short(ch.uuid)}"
        val first = withContext(Dispatchers.Main) {
            val list = c.subs.getOrPut(key) { mutableListOf() }
            list.add(value)
            list.size == 1
        }
        if (first) setNotify(c, ch, true)
        try {
            awaitCancellation()
        } finally {
            main.post {
                val list = c.subs[key]
                list?.remove(value)
                if (list.isNullOrEmpty()) {
                    c.subs.remove(key)
                    AsukaModules.scope.launch { runCatching { setNotify(c, ch, false) } }
                }
            }
        }
    }

    private suspend fun setNotify(c: Conn, ch: BluetoothGattCharacteristic, on: Boolean) {
        withContext(Dispatchers.Main) { c.gatt?.setCharacteristicNotification(ch, on) }
        val d = ch.getDescriptor(CCCD) ?: return
        val v = when {
            !on -> BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE
            ch.properties and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0 && ch.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY == 0 ->
                BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
            else -> BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        }
        c.run<Any?> { writeDescriptor(it, d, v) }
    }

    // Before Android 13 a write takes the characteristic's (the
    // descriptor's) value, and says only whether it started.

    @Suppress("DEPRECATION")
    private fun writeCharacteristic(g: BluetoothGatt, ch: BluetoothGattCharacteristic, value: ByteArray, type: Int): Boolean {
        if (Build.VERSION.SDK_INT >= 33) return g.writeCharacteristic(ch, value, type) == BluetoothStatusCodes.SUCCESS
        ch.writeType = type
        ch.value = value
        return g.writeCharacteristic(ch)
    }

    @Suppress("DEPRECATION")
    private fun writeDescriptor(g: BluetoothGatt, d: BluetoothGattDescriptor, value: ByteArray): Boolean {
        if (Build.VERSION.SDK_INT >= 33) return g.writeDescriptor(d, value) == BluetoothStatusCodes.SUCCESS
        d.value = value
        return g.writeDescriptor(d)
    }

    // The app as a peripheral.

    private var server: BluetoothGattServer? = null
    private val requests = HashMap<Long, Triple<BluetoothDevice, Int, Int>>() // device, requestId, offset
    private var nextRequest = 0L
    private val subscribers = HashMap<UUID, MutableSet<BluetoothDevice>>()
    private val served = HashMap<UUID, BluetoothGattCharacteristic>()

    override suspend fun serve(services: List<ServedService>, request: (Request) -> Unit) {
        ready(peripheral)
        val added = CompletableDeferred<Unit>()
        var left = services.size
        val cb = object : BluetoothGattServerCallback() {
            override fun onServiceAdded(status: Int, s: BluetoothGattService) {
                main.post { if (--left <= 0) added.complete(Unit) }
            }
            override fun onCharacteristicReadRequest(d: BluetoothDevice, id: Int, offset: Int, ch: BluetoothGattCharacteristic) {
                main.post {
                    val n = ++nextRequest
                    requests[n] = Triple(d, id, offset)
                    request(Request(id = n, kind = RequestKind.READ, central = d.address, service = short(ch.service.uuid), characteristic = short(ch.uuid), offset = offset.toLong()))
                }
            }
            override fun onCharacteristicWriteRequest(d: BluetoothDevice, id: Int, ch: BluetoothGattCharacteristic, prepared: Boolean, needsResponse: Boolean, offset: Int, value: ByteArray) {
                main.post {
                    val n = ++nextRequest
                    if (needsResponse) requests[n] = Triple(d, id, offset)
                    request(Request(id = n, kind = RequestKind.WRITE, central = d.address, service = short(ch.service.uuid), characteristic = short(ch.uuid),
                        value = value, offset = offset.toLong()))
                }
            }
            override fun onDescriptorWriteRequest(d: BluetoothDevice, id: Int, desc: BluetoothGattDescriptor, prepared: Boolean, needsResponse: Boolean, offset: Int, value: ByteArray) {
                main.post {
                    if (needsResponse) server?.sendResponse(d, id, BluetoothGatt.GATT_SUCCESS, 0, null)
                    if (desc.uuid != CCCD) return@post
                    val ch = desc.characteristic
                    val on = !value.contentEquals(BluetoothGattDescriptor.DISABLE_NOTIFICATION_VALUE)
                    val set = subscribers.getOrPut(ch.uuid) { HashSet() }
                    if (on) set.add(d) else set.remove(d)
                    request(Request(id = ++nextRequest, kind = if (on) RequestKind.SUBSCRIBE else RequestKind.UNSUBSCRIBE, central = d.address,
                        service = short(ch.service.uuid), characteristic = short(ch.uuid)))
                }
            }
            override fun onConnectionStateChange(d: BluetoothDevice, status: Int, state: Int) {
                if (state == BluetoothProfile.STATE_DISCONNECTED) main.post {
                    for ((u, set) in subscribers) {
                        if (set.remove(d)) {
                            val ch = served[u] ?: continue
                            request(Request(id = ++nextRequest, kind = RequestKind.UNSUBSCRIBE, central = d.address, service = short(ch.service.uuid), characteristic = short(u)))
                        }
                    }
                }
            }
        }
        withContext(Dispatchers.Main) {
            server?.close()
            served.clear()
            subscribers.clear()
            val s = manager!!.openGattServer(ctx, cb) ?: throw BluetoothException(BluetoothError.UNSUPPORTED)
            server = s
            if (services.isEmpty()) added.complete(Unit)
            for (svc in services) {
                val gs = BluetoothGattService(full(svc.uuid), if (svc.primary) BluetoothGattService.SERVICE_TYPE_PRIMARY else BluetoothGattService.SERVICE_TYPE_SECONDARY)
                for (c in svc.characteristics) {
                    var props = 0
                    for (p in c.properties) props = props or when (p) {
                        Property.BROADCAST -> BluetoothGattCharacteristic.PROPERTY_BROADCAST
                        Property.READ -> BluetoothGattCharacteristic.PROPERTY_READ
                        Property.WRITE_WITHOUT_RESPONSE -> BluetoothGattCharacteristic.PROPERTY_WRITE_NO_RESPONSE
                        Property.WRITE -> BluetoothGattCharacteristic.PROPERTY_WRITE
                        Property.NOTIFY -> BluetoothGattCharacteristic.PROPERTY_NOTIFY
                        Property.INDICATE -> BluetoothGattCharacteristic.PROPERTY_INDICATE
                        else -> 0
                    }
                    var perms = 0
                    for (p in c.permissions) perms = perms or when (p) {
                        Permission.READABLE -> BluetoothGattCharacteristic.PERMISSION_READ
                        Permission.WRITEABLE -> BluetoothGattCharacteristic.PERMISSION_WRITE
                        Permission.READ_ENCRYPTED -> BluetoothGattCharacteristic.PERMISSION_READ_ENCRYPTED
                        Permission.WRITE_ENCRYPTED -> BluetoothGattCharacteristic.PERMISSION_WRITE_ENCRYPTED
                        else -> 0
                    }
                    if (perms == 0) perms = BluetoothGattCharacteristic.PERMISSION_READ
                    val ch = BluetoothGattCharacteristic(full(c.uuid), props, perms)
                    if (props and (BluetoothGattCharacteristic.PROPERTY_NOTIFY or BluetoothGattCharacteristic.PROPERTY_INDICATE) != 0) {
                        ch.addDescriptor(BluetoothGattDescriptor(CCCD, BluetoothGattDescriptor.PERMISSION_READ or BluetoothGattDescriptor.PERMISSION_WRITE))
                    }
                    gs.addCharacteristic(ch)
                    served[ch.uuid] = ch
                }
                s.addService(gs)
            }
        }
        added.await()
        try {
            awaitCancellation()
        } finally {
            main.post {
                server?.close()
                server = null
                served.clear()
                subscribers.clear()
            }
        }
    }

    override fun respond(request: Long, value: ByteArray, error: Long) {
        main.post {
            val (d, id, offset) = requests.remove(request) ?: return@post
            val v = if (offset < value.size) value.copyOfRange(offset, value.size) else ByteArray(0)
            server?.sendResponse(d, id, if (error == 0L) BluetoothGatt.GATT_SUCCESS else error.toInt(), offset, if (error == 0L) v else null)
        }
    }

    override fun notify(service: String, characteristic: String, value: ByteArray): Long {
        val u = full(characteristic)
        val ch = served[u] ?: return 0
        val to = subscribers[u]?.toList() ?: return 0
        main.post {
            val indicate = ch.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY == 0
            val s = server ?: return@post
            for (d in to) notifyChanged(s, d, ch, indicate, value)
        }
        return to.size.toLong()
    }

    /** Before Android 13 the value is the characteristic's. */
    @Suppress("DEPRECATION")
    private fun notifyChanged(s: BluetoothGattServer, d: BluetoothDevice, ch: BluetoothGattCharacteristic, indicate: Boolean, value: ByteArray) {
        if (Build.VERSION.SDK_INT >= 33) {
            s.notifyCharacteristicChanged(d, ch, indicate, value)
            return
        }
        ch.value = value
        s.notifyCharacteristicChanged(d, ch, indicate)
    }

    override suspend fun advertise(options: Advertising) {
        ready(peripheral)
        val adv = adapter?.bluetoothLeAdvertiser ?: throw BluetoothException(BluetoothError.UNSUPPORTED)
        val started = CompletableDeferred<Unit>()
        val cb = object : AdvertiseCallback() {
            override fun onStartSuccess(s: AdvertiseSettings) { started.complete(Unit) }
            override fun onStartFailure(code: Int) { started.completeExceptionally(Exception("bluetooth: advertising failed ($code)")) }
        }
        val data = AdvertiseData.Builder().setIncludeTxPowerLevel(options.includeTxPower)
        for (s in options.services) data.addServiceUuid(ParcelUuid(full(s)))
        for ((k, v) in options.serviceData) data.addServiceData(ParcelUuid(full(k)), v)
        for ((k, v) in options.manufacturerData) data.addManufacturerData(k.toInt(16), v)
        val scan = AdvertiseData.Builder()
        if (options.localName.isNotEmpty()) {
            runCatching { adapter?.name = options.localName }
            scan.setIncludeDeviceName(true)
        }
        withContext(Dispatchers.Main) {
            adv.startAdvertising(AdvertiseSettings.Builder().setConnectable(options.connectable).setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_BALANCED).build(),
                data.build(), scan.build(), cb)
        }
        started.await()
        try {
            awaitCancellation()
        } finally {
            main.post { runCatching { adv.stopAdvertising(cb) } }
        }
    }

    private companion object {
        var asks = 0
    }
}

fun makeAdapter(): Adapter = AndroidAdapter(AsukaModules.context)

/** The advertisement a scan result is (its time: when it was heard, which a background scan's batch reports later). */
@SuppressLint("MissingPermission") // the name only with BLUETOOTH_CONNECT granted: else none
internal fun advertisement(ctx: Context, r: ScanResult): Heard {
    val rec = r.scanRecord
    val sd = HashMap<String, ByteArray>()
    rec?.serviceData?.forEach { (k, v) -> sd[short(k.uuid)] = v }
    val md = HashMap<String, ByteArray>()
    rec?.manufacturerSpecificData?.let { m -> for (i in 0 until m.size()) md[String.format("%04x", m.keyAt(i))] = m.valueAt(i) }
    val named = Build.VERSION.SDK_INT < 31 || ctx.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
    val name = (if (named) runCatching { r.device.name }.getOrNull() else null) ?: rec?.deviceName ?: ""
    val ago = SystemClock.elapsedRealtimeNanos() - r.timestampNanos
    return Heard(
        device = Device(id = r.device.address, name = name), rssi = r.rssi.toLong(), localName = rec?.deviceName ?: "",
        services = rec?.serviceUuids?.map { short(it.uuid) } ?: emptyList(),
        solicited = if (Build.VERSION.SDK_INT >= 29) rec?.serviceSolicitationUuids?.map { short(it.uuid) } ?: emptyList()
            else rec?.bytes?.let(::solicited) ?: emptyList(),
        serviceData = sd, manufacturerData = md,
        txPower = (rec?.txPowerLevel ?: Int.MIN_VALUE).let { if (it == Int.MIN_VALUE) 0L else it.toLong() },
        hasTxPower = (rec?.txPowerLevel ?: Int.MIN_VALUE) != Int.MIN_VALUE,
        // Before Android 8 a scan result doesn't say: connectable.
        connectable = Build.VERSION.SDK_INT < 26 || r.isConnectable, time = System.currentTimeMillis() * 1_000_000 - ago.coerceAtLeast(0),
    )
}

/**
 * The solicited services of an advertisement's bytes (its 16-, 32- and
 * 128-bit lists): ScanRecord reads them only from Android 10.
 */
private fun solicited(b: ByteArray): List<String> {
    val out = ArrayList<String>()
    var i = 0
    while (i < b.size) {
        val n = b[i].toInt() and 0xff
        if (n == 0 || i + n >= b.size) break
        val size = when (b[i + 1].toInt() and 0xff) { 0x14 -> 2; 0x1f -> 4; 0x15 -> 16; else -> 0 }
        var j = i + 2
        while (size > 0 && j + size <= i + 1 + n) {
            // Little endian: the last byte first.
            val hex = (j + size - 1 downTo j).joinToString("") { "%02x".format(b[it]) }
            val u = if (size == 16) "${hex.substring(0, 8)}-${hex.substring(8, 12)}-${hex.substring(12, 16)}-${hex.substring(16, 20)}-${hex.substring(20)}"
                else "${hex.padStart(8, '0')}$BASE"
            out.add(short(UUID.fromString(u)))
            j += size
        }
        i += n + 1
    }
    return out
}

/**
 * Android's background scan (backgroundScan): one scan the system runs
 * for the app, reporting to a PendingIntent (BluetoothScanReceiver, which
 * the package's asuka.toml declares only in the apps that call
 * backgroundScan), so it goes on after the app's process has gone. What
 * it hears goes to this process's background scans if it has some, else
 * to watchRestored's readers, else to a file the next reader takes. The
 * PendingIntent is the same each time: a later run's scan replaces what
 * an earlier one left, stopBackgroundScan stops it. Main thread, but for
 * the file.
 */
internal object BackgroundScan {
    private const val ACTION = "asuka.bluetooth.SCAN"
    private const val PREFS = "asuka.bluetooth"
    /** At most this many advertisements wait in the file. */
    private const val KEEP = 1000

    /** A background scan of this process's. */
    class Reader(val services: List<String>, val duplicates: Boolean, val found: (Heard) -> Unit) {
        val seen = HashSet<String>()
        /** Completed (exceptionally) when the system stops the scan. */
        val failed = CompletableDeferred<Unit>()
    }

    private val readers = LinkedHashMap<Long, Reader>()
    private val restorers = LinkedHashMap<Long, (Brought) -> Unit>()
    private var ids = 0L
    /** Whether this process started the scan, and for which services (null: every device). */
    private var running = false
    private var services: Set<String>? = null
    private val file = Any()

    private fun intent(c: Context): PendingIntent = PendingIntent.getBroadcast(c, 0,
        Intent(c, BluetoothScanReceiver::class.java).setAction(ACTION),
        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE) // mutable: the system adds the results

    private fun kept(c: Context) = File(c.noBackupFilesDir, "asuka-bluetooth-restored.json")

    private fun prefs(c: Context) = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    @Suppress("DEPRECATION") // queryBroadcastReceivers' flags as an int, before Android 13
    fun declared(c: Context): Boolean {
        val i = Intent(c, BluetoothScanReceiver::class.java)
        return if (Build.VERSION.SDK_INT >= 33) c.packageManager.queryBroadcastReceivers(i, PackageManager.ResolveInfoFlags.of(0)).isNotEmpty()
            else c.packageManager.queryBroadcastReceivers(i, 0).isNotEmpty()
    }

    /**
     * Adds a background scan: the system's starts, or widens to its
     * services (the readers filter what it hears, so it narrows only as
     * it stops). Throws as it can't start.
     */
    @SuppressLint("MissingPermission") // asked for first (AndroidAdapter.ready)
    @RequiresApi(26)
    fun add(c: Context, scanner: BluetoothLeScanner, r: Reader): Long {
        val want: Set<String>? = if (r.services.isEmpty() || readers.values.any { it.services.isEmpty() }) null
            else (readers.values.flatMap { it.services } + r.services).toSet()
        val have = services
        if (!running || have != null && (want == null || !have.containsAll(want))) {
            // A previous run's scan, or this one's narrower: replaced.
            runCatching { scanner.stopScan(intent(c)) }
            val filters = want?.map { ScanFilter.Builder().setServiceUuid(ParcelUuid(full(it))).build() }
            val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_POWER)
                .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES).build()
            val code = scanner.startScan(filters, settings, intent(c))
            if (code == ScanCallback.SCAN_FAILED_FEATURE_UNSUPPORTED) throw BluetoothException(BluetoothError.UNSUPPORTED)
            if (code != 0) throw Exception("bluetooth: the background scan failed ($code)")
            running = true
            services = want
        }
        val id = ++ids
        readers[id] = r
        // What the receiver keeps with the process gone: one per device, unless one wanted each.
        prefs(c).edit().putBoolean("duplicates", readers.values.any { it.duplicates }).apply()
        return id
    }

    fun remove(c: Context, id: Long) {
        readers.remove(id) ?: return
        if (readers.isEmpty()) stop(c)
    }

    @SuppressLint("MissingPermission")
    private fun stop(c: Context) {
        running = false
        services = null
        // Before Android 8 there is none to stop.
        val scanner = c.getSystemService(BluetoothManager::class.java)?.adapter?.bluetoothLeScanner
        if (Build.VERSION.SDK_INT >= 26) runCatching { scanner?.stopScan(intent(c)) }
        prefs(c).edit().remove("duplicates").apply()
    }

    /** Stops the scan a previous run left; this run's go on. */
    fun stopLeft(c: Context) {
        if (readers.isEmpty()) stop(c)
    }

    fun listenRestored(f: (Brought) -> Unit): Long {
        val id = ++ids
        restorers[id] = f
        return id
    }

    fun unlistenRestored(id: Long) {
        restorers.remove(id)
    }

    /** The receiver's broadcast: what the scan heard, or why it stopped. done once it's handled. */
    fun received(c: Context, i: Intent, done: () -> Unit) {
        // Only Android 8 and later's scans send it.
        if (Build.VERSION.SDK_INT < 26) return done()
        val err = i.getIntExtra(BluetoothLeScanner.EXTRA_ERROR_CODE, 0)
        if (err != 0) {
            if (readers.isNotEmpty()) {
                running = false
                services = null
                val e = Exception("bluetooth: the background scan stopped ($err)")
                for (r in readers.values) r.failed.completeExceptionally(e)
            }
            return done()
        }
        val rs = IntentCompat.getParcelableArrayListExtra(i, BluetoothLeScanner.EXTRA_LIST_SCAN_RESULT, ScanResult::class.java)
        if (rs.isNullOrEmpty()) return done()
        val ads = rs.map { advertisement(c, it) }
        if (readers.isNotEmpty()) {
            for (a in ads) for (r in readers.values.toList()) {
                if (r.services.isNotEmpty() && r.services.none { it in a.services }) continue
                if (!r.duplicates && !r.seen.add(a.device.id)) continue
                r.found(a)
            }
            return done()
        }
        if (restorers.isNotEmpty()) {
            val r = Brought(advertisements = ads)
            for (f in restorers.values.toList()) f(r)
            return done()
        }
        val dup = prefs(c).getBoolean("duplicates", false)
        thread(name = "asuka-bluetooth-keep") {
            try {
                keep(c, ads, dup)
            } finally {
                done()
            }
        }
    }

    /** Adds to the file: one per device (its latest) unless duplicates, the latest KEEP. */
    private fun keep(c: Context, ads: List<Heard>, duplicates: Boolean): Unit = synchronized(file) {
        val f = kept(c)
        val all = ArrayList<JSONObject>()
        if (f.exists()) runCatching { JSONArray(f.readText()) }.getOrNull()?.let { a -> for (i in 0 until a.length()) a.optJSONObject(i)?.let { all.add(it) } }
        for (a in ads) {
            if (!duplicates) all.removeAll { it.optString("id") == a.device.id }
            all.add(encode(a))
        }
        val tmp = File(f.path + ".tmp")
        runCatching {
            tmp.writeText(JSONArray(all.takeLast(KEEP)).toString())
            tmp.renameTo(f)
        }
        Unit
    }

    /** What the file kept, which it forgets. Not on the main thread. */
    fun takeKept(c: Context): List<Heard> = synchronized(file) {
        val f = kept(c)
        if (!f.exists()) return emptyList()
        val a = runCatching { JSONArray(f.readText()) }.getOrNull()
        f.delete()
        if (a == null) return emptyList()
        (0 until a.length()).mapNotNull { a.optJSONObject(it)?.let(::decode) }
    }

    private fun b64(b: ByteArray) = Base64.encodeToString(b, Base64.NO_WRAP)

    private fun encode(a: Heard): JSONObject = JSONObject()
        .put("id", a.device.id).put("name", a.device.name).put("rssi", a.rssi).put("localName", a.localName)
        .put("services", JSONArray(a.services)).put("solicited", JSONArray(a.solicited))
        .put("serviceData", JSONObject().apply { for ((k, v) in a.serviceData) put(k, b64(v)) })
        .put("manufacturerData", JSONObject().apply { for ((k, v) in a.manufacturerData) put(k, b64(v)) })
        .put("txPower", a.txPower).put("hasTxPower", a.hasTxPower).put("connectable", a.connectable).put("time", a.time)

    private fun decode(o: JSONObject): Heard {
        fun strings(k: String) = o.optJSONArray(k)?.let { a -> (0 until a.length()).map { a.optString(it) } } ?: emptyList()
        fun bytes(k: String) = o.optJSONObject(k)?.let { m -> m.keys().asSequence().associateWith { Base64.decode(m.optString(it), Base64.NO_WRAP) } } ?: emptyMap()
        return Heard(
            device = Device(id = o.optString("id"), name = o.optString("name")), rssi = o.optLong("rssi"), localName = o.optString("localName"),
            services = strings("services"), solicited = strings("solicited"), serviceData = bytes("serviceData"), manufacturerData = bytes("manufacturerData"),
            txPower = o.optLong("txPower"), hasTxPower = o.optBoolean("hasTxPower"), connectable = o.optBoolean("connectable", true), time = o.optLong("time"),
        )
    }
}

/**
 * The background scan's results (a PendingIntent scan the system runs,
 * which outlives the process): BackgroundScan's. It doesn't start the
 * app's code: the system may start the process only for this, and what
 * it heard waits for the next run's restored().
 */
class BluetoothScanReceiver : BroadcastReceiver() {
    override fun onReceive(c: Context, i: Intent) {
        val hold = goAsync()
        BackgroundScan.received(c.applicationContext, i) { hold.finish() }
    }
}
