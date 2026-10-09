# Contributing to gcan-usbcanfd

Thanks for helping improve the Linux driver for the GCAN USBCANFD adapter (USB `0c66:000e`).

## License
This project is licensed under the GNU General Public License v2.0 (see [LICENSE](LICENSE)). By submitting a contribution
(pull request or patch) you agree that it is licensed under the same terms.

## Reporting issues
Please include:
- kernel version (`uname -r`) and distribution
- `lsusb | grep 0c66` and the output of `ip -details link show can0`
- `journalctl -k | grep -i gcan` (the driver logs when it binds and on bus-off)
- what you ran (bitrate, `fd on` or off, what was connected) and what you expected

## Building and testing
    make              # needs the kernel headers for the running kernel
    make load         # loads the module (asks for sudo)
    make unload

The driver targets kernel 6.8. Hardware is needed for most checks; the status table in the README lists what has been
tested, and [docs/TODO.md](docs/TODO.md) lists what has not.

## Pull requests
- Keep changes small and say how you tested them (hardware, kernel, bitrates).
- Follow the existing kernel coding style (tabs, 80 to 100 columns) and keep the file header at the top of new files.
- Update `docs/PROTOCOL.md` when you learn something new about the USB protocol, and mention what you observed it from.
