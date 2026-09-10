# CANable 2.0 Firmware

> **说明**: 本固件已从 slcan ASCII 切换到二进制 USB 协议, 命令与格式详见
> **[docs/FIRMWARE_PROTOCOL.md](docs/FIRMWARE_PROTOCOL.md)** (固件说明书)。
> 下述 Supported Commands 为历史版本记录, 可能已过时。

This repository contains sources for the slcan CANable 2.0 firmware. This firmware implements non-standard slcan commands to support CANFD messaging alongside a LAWICEL-style command set.

## Supported Commands

- `O` - Open channel 
- `C` - Close channel 
- `S0` - Set nominal bitrate to 10k
- `S1` - Set nominal bitrate to 20k
- `S2` - Set nominal bitrate to 50k
- `S3` - Set nominal bitrate to 100k
- `S4` - Set nominal bitrate to 125k
- `S5` - Set nominal bitrate to 250k
- `S6` - Set nominal bitrate to 500k
- `S7` - Set nominal bitrate to 750k
- `S8` - Set nominal bitrate to 1M
- `S9` - Set nominal bitrate to 83.3k
- `Y2` - Set data bitrate to 2M (CANFD only) (default)
- `Y5` - Set data bitrate to 5M (CANFD only)
- `M0` - Set mode to normal mode (default)
- `M1` - Set mode to silent mode
- `A0` - Disable automatic retransmission 
- `A1` - Enable automatic retransmission (default)
- `tIIILDD...` - Transmit data frame (Standard ID) [ID, length, data]
- `TIIIIIIIILDD...` - Transmit data frame (Extended ID) [ID, length, data]
- `RIIIIIIIIL` - Transmit remote frame (Extended ID) [ID, length]
- `rIIIL` - Transmit remote frame (Standard ID) [ID, length]
- `dIIILDD...` - Transmit CAN FD standard ID (no BRS) [ID, length]
- `DIIIIIIIILDD...` - Transmit CAN FD extended ID (no BRS) [ID, length]
- `bIIILDD...` - Transmit CAN FD BRS standard ID [ID, length]
- `BIIIIIIIILDD...` - Transmit CAN FD extended ID [ID, length]

- `V` - Returns firmware version and remote path as a string
- `E` - Returns error register

Note: CANFD message lengths are as follows (expressed in hexadecimal):
- `0-8`: Same as standard CAN
- `9`: Length = 12
- `A`: Length = 16
- `B`: Length = 20
- `C`: Length = 24
- `D`: Length = 32
- `E`: Length = 48
- `F`: Length = 64

Note: Channel configuration commands must be sent before opening the channel. The channel must be opened before transmitting frames.

This firmware currently does not provide any ACK/NACK feedback for serial commands.

## Building

Firmware builds with GCC. Specifically, you will need gcc-arm-none-eabi, which
is packaged for Windows, OS X, and Linux on
[Launchpad](https://launchpad.net/gcc-arm-embedded/+download). Download for your
system and add the `bin` folder to your PATH.

Your Linux distribution may also have a prebuilt package for `arm-none-eabi-gcc`, check your distro's repositories to see if a build exists. Simply compile by running `make`. 

## Building with Keil MDK-ARM (uVision)

An Arm Compiler 6 (armclang) uVision project is provided at
`MDK-ARM/canable2.uvprojx` (device N32H473CE, MicroLIB). Open it in Keil and
Build (F7), or build from the command line:

    UV4.exe -j0 -b MDK-ARM\canable2.uvprojx -o build.log

Outputs go to `MDK-ARM/Objects/` (`canable2.axf`, `canable2.bin`,
`canable2.hex`); an `AfterMake` step runs `fromelf --bin` to produce the `.bin`.

### Warning policy (Keil)

The Keil project is pinned to the same warning baseline as the GCC build
(`-Wall`), so both builds report the same diagnostics:

- `Cads/wLevel = 0` -- Keil does NOT inject the extra `-Wextra` /
  `-Wconversion` / `-Wpadded` families.
- `Cads/MiscControls = -include git_version.h -Wall` -- enables `-Wall`
  explicitly, and force-includes `git_version.h` (which supplies the
  `GIT_VERSION` / `GIT_REMOTE` string macros for `main.c`; uVision's Define
  field strips embedded quotes, so a force-include is used instead).
- `src/` must stay `-Wall`-clean. It currently is (one unused variable in
  `usb_pwr.c` was removed for this).
- A vendor warning that survives `-Wall` is silenced only on that file, via its
  `FileOption` -- e.g. `Drivers/.../n32h47x_48x_rtc.c` sets
  `-Wno-unused-variable` (unused `temp`).

Do NOT raise `wLevel` back to 2 (the Keil "All Warnings" preset): it turns on
`-Wextra` / `-Wconversion` / `-Wpadded` / `-Wcovered-switch-default` etc. across
the vendored `Drivers/`, producing ~199 warnings.

## Flashing with the Bootloader

Plug in your CANable2 while pressing down the BOOT button. The blue LED should be dimly illuminated. Next, type `make flash` and your CANable will be updated to the latest firwmare. Unplug/replug the device after moving the boot jumper back, and your CANable will be up and running.


## License

See LICENSE.md
