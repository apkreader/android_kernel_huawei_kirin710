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

## Status

Not built or boot-tested for this device. Android 17 compatibility and
current-toolchain support still need to be implemented. KernelSU is not
included.

Huawei's original build notes are in `README_Kernel.txt`. They expect the
older `aarch64-linux-android-4.9` toolchain and produce `Image.gz`. The phone
uses a Huawei kernel container; the raw output is not a flashable image.

## License

See `COPYING` and the notices in individual files.
