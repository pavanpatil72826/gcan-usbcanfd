# GCAN USBCANFD USB protocol (0c66:000e)

Reverse engineered from USBPcap captures of the vendor Windows driver and checked against the real adapter from Linux.
All multi-byte integers are little endian. Status: everything below was observed on the wire unless marked *unknown*.

## USB layout
One configuration, one interface (class 0xDC, vendor specific), five bulk endpoints with 512 byte packets.
No vendor control requests are used. Do not send an extra SET_CONFIGURATION: the device then drops the first command ack.

| Endpoint | Direction | Use |
|---|---|---|
| 0x02 | OUT | commands |
| 0x82 | IN | command ack (the command is echoed back) |
| 0x81 | IN | receive stream, host keeps 1024 byte reads pending |
| 0x01 | OUT | transmit, always a 1024 byte zero padded transfer |
| 0x83 | IN | transmit ack |

## Commands (EP 0x02 out, echoed on EP 0x82)
Order after plug-in: time sync, then per channel init, start. On close: stop, reset.

| Command | Bytes |
|---|---|
| time sync | `aa 55 07 <year u16> <month> <day> <hour> <min> <sec>` (10 bytes, echoed) |
| init channel | 80 bytes, zero padded, echoed (see below) |
| start channel | `02 <ch> aa 55` |
| stop channel | `03 <ch> aa 55` |
| reset channel | `01 <ch> aa 55` |

Init packet: `01 <ch> <rxmode> <txmode> <nominal bps u32> <data bps u32> <filter used> <std/ext> <nominal sel> <data sel> <16 x u32 filters>`
- `rxmode` = 3 (receive all standard and extended ids), `txmode` = 0 (POSITIVE_SEND) when transmitting. 1 (PASSIVE_SEND) also reaches the bus.
- Bitrates are plain bits per second **and** a table index. Both are sent; the index is what the vendor header enumerates.
- Nominal index: 1M, 800K, 500K, 400K, 250K, 200K, 125K, 100K, 80K, 62500, 50K, 40K, 25K, 20K, 10K, 5K.
- Data index: 5M, 4M, 2M, 1M, 800K, 500K, 400K, 250K, 200K, 125K, 100K, 80K, 62500, 50K, 40K, 25K, 20K, 10K, 5K.
- Example CAN0 500K / 2M: `01 00 03 00 20a10700 80841e00 00 00 02 02 ...`.
- Example CAN1 1M / 5M: `01 01 03 01 40420f00 404b4c00 00 00 00 00 ...`.

## Frame records (same layout for receive and transmit)
`<len> <ch> <timestamp 8 bytes> <flags> <dlen> <id u32> <data[dlen]>`, with `len = 16 + dlen`.
- Receive: a 1024 byte read holds back-to-back records; stop at a zero length byte. The timestamp is `mday hour min sec ms(u16) us(u16)`.
- Transmit: timestamp is all zero. One or more records are written into a single 1024 byte transfer, padded with zeros.
- flags: bit0 CAN FD, bit1 extended id, bit2 RTR, bit3 bit-rate switch.
- A record with `len == 30` is a channel status record (about one per millisecond), see below.

## Status records (len 30)
`1e <type> <timestamp 8> <ECR u32> <PSR u32> <20 more bytes>`. Records arrive in triplets with the same timestamp, types 0, 1 and 3.
- type 0 / 1: status of channel 0 / 1 (type 1 stays all zero while channel 1 is not started).
- type 3: 8 pseudo-random bytes at offset 10, rest zero. *Unknown*, ignored.
- ECR (offset 10) and PSR (offset 14) are the Bosch M_CAN registers, as in the vendor header's `ERR_FRAME`:
  ECR: TEC = byte 10, REC = byte 11 bits 0..6, RP = byte 11 bit 7, CEL = byte 12.
  PSR: LEC bits 0..2, ACT bits 3..4 (0 sync, 1 idle, 2 receiver, 3 transmitter), EP bit 5, EW bit 6, BO bit 7, DLEC bits 8..10, RESI bit 11, RBRS bit 12, RFDF bit 13.
- Seen in captures: PSR 0x3017 (receiving FD+BRS), 0x300f (idle), 0x070f (reset value), ECR always 0 (no errors on the bus).
- Verified on hardware with the radar off: the first unacknowledged frame gives TEC = 128 (stays there) and EP set; after the radar
  powered up with our frames still queued the channel reached bus-off (TEC 248). A stop, reset, init, start sequence recovers it.
- Bytes 18..29 hold further counters (vendor `CANFD_STATUS`: RxLost, TxFail, load rate); sparse non-zero values at 26..27 are not decoded.

## Transmit ack (EP 0x83)
Four bytes `aa 55 <n> 00`, about 0.4 ms after the write. `n` is the number of frames accepted (observed with txmode 0).

## Unknown / not captured
- Bytes 18..29 of the status record (lost / failed counters, bus load) and the type 3 record.
- RTR transmit layout (the driver sends no data bytes).
- Acceptance filter configuration, listen-only and other modes.
- Loopback: one acknowledged frame was not echoed on the receive stream; other cases (unacknowledged, classic, long FD) are untested.
- Channel 1 beyond the init and start commands.
