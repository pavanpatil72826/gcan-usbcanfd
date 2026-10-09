# Pending tests and open items

Everything not listed here has been tested on hardware (see the status table in the README).

## Needs a test (on hold)
These need a bus where both ends are under our control: disconnect the radars and join the two connectors of the adapter
(CAN-H to CAN-H, CAN-L to CAN-L, check termination in the adapter manual), bring both channels up at the same bitrate and
send from one channel while running `candump` on the other. Compare the bytes in both directions.

- [ ] CAN FD frames longer than 8 bytes, transmit direction (12, 16, 32, 64) with and without bit-rate switch. Receive of 16, 24 and 64 byte frames with BRS is verified.
- [ ] Extended (29-bit) IDs, classic and FD
- [ ] RTR frames (the driver sends no data bytes; the real transmit layout was never captured)
- [ ] FD with a different data bitrate than 2M, and other nominal bitrates from the table (only 500K and 500K/2M were run)
- [ ] Reboot with the adapter plugged in: `can0` / `can1` should be present after login (plug-in loading is tested, reboot is not)
- [ ] Unplug while transmitting heavily, and while a channel is in bus-off

## Open items (not tests)
- [ ] After the transmit batching rewrite: unplug/replug still to be re-run (the teardown code changed). The down/up cycle, a burst into a stalled channel (110 of 120 accepted, 10 refused, no hang), bus-off and `restart` were re-run and pass.
- [ ] One receive drop (`rx_dropped` on can0: 1 after the 550 frames/s stress run, a few more around each bus-off restart); unexplained, not growing during normal receive
- [ ] Status record bytes 18..29 (lost / failed counters, bus load) and the type 3 record are not decoded; see `docs/PROTOCOL.md`
- [ ] Acceptance filters and listen-only mode are not implemented (the init packet has fields for filters)
- [ ] Hardware timestamps are ignored (receive records carry them)
- [ ] The module is installed for one kernel version; DKMS would rebuild it after kernel updates
