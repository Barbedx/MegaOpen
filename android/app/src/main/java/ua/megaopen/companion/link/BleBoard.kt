package ua.megaopen.companion.link

import java.util.UUID

/**
 * The board on Bluetooth LE, what both sides agree on: one GATT service whose
 * two characteristics carry the same Frames as the USB vendor interface
 * (docs/design/usb-link.md, «Bluetooth»). Pure: unit-tested in BleBoardTest.
 */
object BleBoard {
    val SERVICE: UUID = UUID.fromString("4d4f0100-6c69-6e6b-8000-4d6567614f70")
    /** App to board: write with response, on an encrypted (bonded) link. */
    val RX: UUID = UUID.fromString("4d4f0101-6c69-6e6b-8000-4d6567614f70")
    /** Board to app: notify. */
    val TX: UUID = UUID.fromString("4d4f0102-6c69-6e6b-8000-4d6567614f70")
    val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    /** The ATT MTU asked for after connecting: 244 bytes of payload per write. */
    const val MTU = 247

    /**
     * A scan result or a bonded device that may be the board: the primary
     * advertising name is always "MEGA"; the scan response says "MegaOpen…"
     * in most of the board's Bluetooth modes; in the link-only mode the
     * advertising carries [SERVICE]. [known] is the board's name as the app
     * already met it (its HELLO, its WiFi status), "" when none.
     */
    fun mayBe(name: String?, uuids: Collection<UUID>, known: String): Boolean {
        if (SERVICE in uuids) return true
        val n = name?.trim().orEmpty()
        if (n.isEmpty()) return false
        return n == "MEGA" || n.contains("megaopen", ignoreCase = true) ||
            (known.isNotEmpty() && n.equals(known, ignoreCase = true))
    }

    /** The next pause between connection tries: 2 s, doubling, at most 30 s. */
    fun backoff(prevMs: Long): Long = if (prevMs <= 0) FIRST_BACKOFF_MS else minOf(prevMs * 2, MAX_BACKOFF_MS)

    const val FIRST_BACKOFF_MS = 2_000L
    const val MAX_BACKOFF_MS = 30_000L
}
