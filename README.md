# FXDK

FXDK is a lower filter driver for disk device to make removable disk device reported to system as fixed disk device.

This is basically a rewrite of the cfadisk driver from HGST with KMDF.

The original cfadisk driver has a bug related to missing of remove lock of PNP handling. System will BSOD when lower device (eg. USB device) getting removed by pulling out (so called surprise removal).

By utilizing KMDF, it has some advantages over the cfadisk driver.

* Much simpler code
* Get correct PNP handling for free, no longer need to worry about remove lock

## Usecase

### HyperV

HyperV only support passthrough physical disk that can be offlined to virtual machine, however, Windows does not support offline removable disk device.

By reporting the disk device as fixed disk device, removable device can be offlined and attach to virtual machine.

## Tested Windows version

* AMD64 10.0.26100.1 (Windows 11 24H2)

## Installation

fxdk.inf will neither install the driver as lower filter for disk class nor for individual device.

This is by design to make it more flexible for different usecases.

### Install for disk class

Append 'fxdk' to `HKEY_LOCAL_MACHINE\SYSTEM\ControlSet001\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}\LowerFilters` registry value.

### Install for specific device

Find the instance path of the device, then append (or create if not exists) 'fxdk' to `HKEY_LOCAL_MACHINE\SYSTEM\ControlSet001\Enum\`_instance path_`\LowerFilters` registry value.

Note, `LowerFilters` value is a `REG_MULTI_SZ`.

fxdk will not participate the stack if the lower device does not have `FILE_REMOVABLE_MEDIA` PDO device flag.

## Build

You can build it using VS2022 with WDK installed or EWDK.

For EWDK use
```
msbuild .\fxdk.sln  /p:Configuration=Debug /p:Platform="x64"
```
to build. Adjust Configuration and Platform for your needs.

If you are building for x86, you need older WDK (at least older than 22H2 (10.0.22621.1)). 22H2 WDK does have all required libraries for x86 but it has a check to prevent building for x86 or ARM.

Also, driver signing is disabled by default. You need configure it for yourself.
