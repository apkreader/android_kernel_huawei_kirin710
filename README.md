# Huawei Kirin 710 kernel

Huawei's EMUI 9.1 Linux 4.9.148 source, used as the starting point for the
P30 lite (marie) LineageOS 24.0 port.

## Source

Imported from [Iceows/android_kernel_huawei_kirin710-9](https://github.com/Iceows/android_kernel_huawei_kirin710-9),
commit `892c779d0a134ca21d7212bdcf1bc5d00dd1edcf` (the initial Huawei source
import). The release notes date the package to 2019-06-28. Upstream authorship
and license files are preserved.

`emui-9.1` retains that import. Device changes are on `lineage-24.0`.

`arch/arm64/configs/marie_defconfig` was captured from `/proc/config.gz` on a
MAR-LX1B / MAR-L21B running EMUI 9.1.0.399 (C431), kernel 4.9.148. The source
release predates that firmware; matching version numbers do not establish
that the sources and device configuration are an exact match.

The public build disables Huawei CFI, RandStruct and StructLeak compiler
plugins that are absent from the AOSP toolchain. Module signing uses a key
generated in the build output directory.

## Status

This branch matches the kernel source and configuration used for the
LineageOS 24 development build. The phone has booted Android and passed
the WPA2 Wi-Fi test with this kernel. The ROM also uses private init and
system changes. Bluetooth and cellular are still being brought up.
KernelSU is not included.

`MADV_WIPEONFORK` and `MADV_KEEPONFORK` are backported from upstream Linux.
On-device tests cover child memory clearing, repeated forks, parent isolation,
restoring inheritance, and rejecting shared or file-backed mappings.

Huawei's original build notes are in `README_Kernel.txt`. They expect the
older `aarch64-linux-android-4.9` toolchain and produce `Image.gz`. The phone
uses a Huawei kernel container; the raw output is not a flashable image.

## License

See `COPYING` and the notices in individual files.
