# gcan-usbcanfd

Linux SocketCAN kernel driver for the GCAN USBCANFD adapter (USB `0c66:000e`). CAN and CAN FD show up as `can0` / `can1`;
no vendor library is needed. GCAN ships only Windows libraries, so the USB protocol was reverse engineered from captures of
the vendor driver (see [docs/PROTOCOL.md](docs/PROTOCOL.md)). Plain C, out-of-tree module, targets kernel 6.8.

**Status: early development.**

| | |
|---|---|
| Builds and loads on 6.8 | yes |
| Plug-in creates `can0` / `can1`, `ip link set up` works | yes |
| Bitrate tables shown by `ip -details link` | yes |
| Receive in `candump` (CAN FD, 500K/2M, ~840 frames/s, no drops) | yes, tested on hardware |
| Transmit with `cansend` and python-can (8 B FD+BRS, 20 ms cyclic) | yes, tested on hardware |
| `ip link set can0 down` / up cycle | yes, receive and transmit recover |
| Unplug while up, then replug | yes, clean disconnect, driver rebinds on its own |
| Error counters, ERROR-PASSIVE with radar off, BUS-OFF, `ip link ... type can restart` recovery | yes, tested on hardware |
| Classic CAN receive (`fd off`, 500K, 82 standard IDs, ~1640 frames/s, no errors or drops) | yes, tested on hardware |
| Classic CAN transmit (cyclic speed / yaw / gear, 110 frames/s, radar echoes the values) | yes, tested on hardware |
| CAN1 (second connector): classic receive and cyclic transmit, radar echoes the values, channels stay separate | yes, tested on hardware |
| `make install`: module loads by itself when the adapter is plugged in, then works as before | yes, tested on hardware |
| FD receive of 16, 24 and 64 byte frames with BRS (a second CAN FD radar: 6 IDs at 20 Hz, no lost cycles, no error events, 1320 frames decoded against its DBC) | yes, tested on hardware |
| Both channels receiving at once (FD radar on can0, classic radar on can1: 141 + 1641 frames/s, no frames crossing channels, 0 errors, 0 drops, all frames decode) | yes, tested on hardware |
| Transmit on one channel (cyclic inputs, 110 frames/s) while the other receives an FD stream: no transmit errors, no lost receive cycles (239 of 239) | yes, tested on hardware |
| Radar with CAN FD firmware on can0 + radar with classic firmware on can1: receive (840 and 1640 frames/s, all frames decode against their DBCs, 0 errors), cyclic inputs on both, radar echoes speed and yaw, read-only radar ID query answered | yes, tested on hardware |
| Both channels transmitting at the same time: 110 frames/s each (0 drops; the earlier one-frame-per-write version dropped ~15%) and 550 frames/s each (0 drops), frames batched into shared 1024-byte writes | yes, tested on hardware |
| FD *transmit* of frames > 8 B, extended IDs, RTR | not tested yet |
| Acceptance filters, hardware timestamps | not implemented |

## Build and load
    make
    make load        # modprobe can, can_raw, can_dev; insmod gcan_usbcanfd.ko  (asks for sudo)
    make unload

`make install` copies the module to `/lib/modules/$(uname -r)/extra/` and runs `depmod`, after which plugging in the
adapter (or `sudo modprobe gcan_usbcanfd`) loads it automatically. Needs the kernel headers (`linux-headers-$(uname -r)`).
Redo `make install` after a kernel update.

## Use
    dmesg | tail                    # "GCAN USBCANFD ready: can0, can1"
    sudo ip link set can0 up type can bitrate 500000 dbitrate 2000000 fd on
    candump can0
    cansend can0 123##1112233445566778
    sudo ip link set can0 down

Only the adapter's own bitrate table is accepted (nominal 1M..5K, data 5M..5K); other values are rejected by `ip link`.
Without `fd on` the data rate is set equal to the nominal rate.

## Notes
- Channels: Linux `can0` is device channel index 0 (called channel 1 by the vendor documents), `can1` is index 1 (channel 2).
  Both channels work. Frames received on one channel appear only on that interface, and transmit goes out on the interface used.
- With no node on the bus to acknowledge frames, the device keeps retrying and stops accepting writes after ~5 frames. The driver then returns
  `ENOBUFS` to the sender and stays alive. The channel goes `ERROR-PASSIVE` (TEC 128) and can reach `BUS-OFF`; recover with
  `ip link set can0 type can restart` (or down and up).
- Do not use a userspace libusb tool on the adapter while this driver is loaded: tools that call `detach_kernel_driver` will remove it from the device.
- RTR frames are sent without data bytes; the transmit layout for RTR was never captured.
- The device does not loop back transmitted frames on the receive stream (checked with one frame). `candump` shows each sent frame once, from the normal SocketCAN local echo.

Pending tests and open items: [docs/TODO.md](docs/TODO.md).

## License
GPL-2.0, see [LICENSE](LICENSE). The kernel exports its CAN and USB interfaces only to GPL modules, so the driver has to be GPL.
Contributions: see [CONTRIBUTING.md](CONTRIBUTING.md).
