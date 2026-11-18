package dev.asuka

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer

// The Kotlin reader of a DevError command's arguments agrees with Go's
// (ops/deverror.go): u8 flags, then four u32-length UTF-8 strings.
class DevErrorTest {
    @Test
    fun decodesTheWireLayout() {
        // ops.AppendDevError(nil, DevError{Flags: DevErrorReload, Title: "T", Message: "m", Details: "d"})
        val b = byteArrayOf(1, 1, 0, 0, 0, 'T'.code.toByte(), 1, 0, 0, 0, 'm'.code.toByte(), 0, 0, 0, 0, 1, 0, 0, 0, 'd'.code.toByte())
        val e = decodeDevError(ByteBuffer.wrap(b))
        assertEquals(AsukaDevError(AsukaDevError.RELOAD, "T", "m", "", "d"), e)
        assertTrue(e!!.reload)
        assertFalse(e.build)
        // Cut short: nothing.
        assertNull(decodeDevError(ByteBuffer.wrap(b, 0, b.size - 1).slice()))
        assertNull(decodeDevError(ByteBuffer.allocate(0)))
    }

    @Test
    fun roundTrips() {
        val e = AsukaDevError(AsukaDevError.BUILD, "Build failed", "app.asuka:6:32: += doesn't take an int and a string",
            "  5 |   Button(\"Add\") {\n> 6 |     n += \"x\"\n    |          ^^^", "Grüße\n\tat explode (app.asuka:12)")
        val d = decodeDevError(ByteBuffer.wrap(encodeDevError(e)))
        assertEquals(e, d)
        assertTrue(d!!.build)
        assertFalse(d.reload)
    }
}
