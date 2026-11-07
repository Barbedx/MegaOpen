package ua.megaopen.companion.link

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.UUID

class BleBoardTest {
    @Test fun recognisesTheBoard() {
        assertTrue(BleBoard.mayBe("MEGA", emptyList(), ""))
        assertTrue(BleBoard.mayBe("MegaOpen", emptyList(), ""))
        assertTrue(BleBoard.mayBe("megaopen-8428", emptyList(), ""))
        assertTrue(BleBoard.mayBe(null, listOf(BleBoard.SERVICE), ""))
        assertTrue(BleBoard.mayBe("Car-1", emptyList(), "car-1"))
    }

    @Test fun ignoresOthers() {
        assertFalse(BleBoard.mayBe(null, emptyList(), ""))
        assertFalse(BleBoard.mayBe("", emptyList(), ""))
        assertFalse(BleBoard.mayBe("Megane speaker", emptyList(), ""))
        assertFalse(BleBoard.mayBe("mega", emptyList(), ""))
        assertFalse(BleBoard.mayBe("Phone", listOf(UUID.fromString("0000180f-0000-1000-8000-00805f9b34fb")), ""))
    }

    @Test fun backoffDoublesToThirtySeconds() {
        val seq = generateSequence(BleBoard.backoff(0)) { BleBoard.backoff(it) }.take(7).toList()
        assertEquals(listOf(2_000L, 4_000L, 8_000L, 16_000L, 30_000L, 30_000L, 30_000L), seq)
    }
}
