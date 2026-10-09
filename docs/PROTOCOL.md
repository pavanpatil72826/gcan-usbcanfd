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
- A record with `len == 30` is a periodic bus status record. Its layout is *unknown* and the driver skips it.

## Transmit ack (EP 0x83)
Four bytes `aa 55 <n> 00`, about 0.4 ms after the write. `n` is the number of frames accepted (observed with txmode 0).

## Unknown / not captured
- Status record layout (bus-off, error counters), error frames.
- RTR transmit layout (the driver sends no data bytes).
- Acceptance filter configuration, listen-only and other modes.
- Whether the device loops back transmitted frames on the receive stream.
- Channel 1 beyond the init and start commands.
