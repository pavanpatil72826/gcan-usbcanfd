# Pending tests and open items

Everything not listed here has been tested on hardware (see the status table in the README).

## Needs a test (on hold)
These need a bus where both ends are under our control: disconnect the radars and join the two connectors of the adapter
(CAN-H to CAN-H, CAN-L to CAN-L, check termination in the adapter manual), bring both channels up at the same bitrate and
send from one channel while running `candump` on the other. Compare the bytes in both directions.

- [ ] CAN FD frames longer than 8 bytes (12, 16, 32, 64) with and without bit-rate switch
- [ ] Extended (29-bit) IDs, classic and FD
- [ ] RTR frames (the driver sends no data bytes; the real transmit layout was never captured)
- [ ] FD with a different data bitrate than 2M, and other nominal bitrates from the table (only 500K and 500K/2M were run)
- [ ] Reboot with the adapter plugged in: `can0` / `can1` should be present after login (plug-in loading is tested, reboot is not)
- [ ] Unplug while transmitting heavily, and while a channel is in bus-off
- [ ] Two channels transmitting at the same time (each was tested alone)

## Open items (not tests)
- [ ] Status record bytes 18..29 (lost / failed counters, bus load) and the type 3 record are not decoded; see `docs/PROTOCOL.md`
- [ ] The transmit path sends one frame per 1024-byte USB write; batching would raise the maximum frame rate
- [ ] Acceptance filters and listen-only mode are not implemented (the init packet has fields for filters)
- [ ] Hardware timestamps are ignored (receive records carry them)
- [ ] The module is installed for one kernel version; DKMS would rebuild it after kernel updates
- [ ] Licence: the repo template ships a non-commercial licence, the module declares GPL (needed for the CAN core symbols). Decide and make them consistent.
- [ ] Interface names: Linux `can0` / `can1` are the vendor's channels 1 / 2. A udev rule could name them after the connectors.
