#!/usr/bin/env bash
# Builds the kernel the way the nightly automation judges a branch.
#
# defconfig alone compiles almost none of the Edera series: Hyper-V nesting,
# Xen dom0 and backends, the PV-IOMMU, OpenPaX and the NUMA-aware Xen paths
# are all off in it. This turns them on as built-ins and builds vmlinux, so a
# replay that breaks any of them, or links against a symbol upstream removed,
# fails here. Building only a subdirectory would miss those link errors.
#
# The rebase skill runs this same script before they finish, so
# what Claude checks and what the workflow checks cannot drift apart.
#
# Usage:
#   kernel-build.sh <x86_64|arm64> <objdir> [jobs]
#
# Run from the top of a kernel tree. <objdir> is created if missing and may be
# reused between runs. For arm64 it uses CROSS_COMPILE (default
# aarch64-linux-gnu-) unless LLVM is set, in which case it builds with clang.
#
# Exit status: make's on a build failure; 3 if an option it asked for did not
# survive olddefconfig (a Kconfig dependency the series no longer satisfies).

set -euo pipefail

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
	echo "usage: $0 <x86_64|arm64> <objdir> [jobs]" >&2
	exit 2
fi
arch=$1
objdir=$2
jobs=${3:-$(nproc)}

common=(
	XEN XEN_BACKEND XEN_DOM0 XEN_BALLOON XEN_GNTDEV XEN_GRANT_DEV_ALLOC
	NUMA ACPI_NUMA MEMORY_HOTPLUG MEMORY_HOTREMOVE ZONE_DEVICE
	XEN_UNPOPULATED_ALLOC XEN_BACKEND_NUMA_AFFINITY
	XEN_NETDEV_FRONTEND XEN_NETDEV_BACKEND XEN_BLKDEV_FRONTEND XEN_BLKDEV_BACKEND
	NET_9P NET_9P_XEN 9P_FS
	DRM DRM_VIRTIO_GPU FW_CFG_SYSFS
	SECURITY OPENPAX OPENPAX_SOFTMODE OPENPAX_XATTR_PAX_FLAGS OPENPAX_MPROTECT
	OPENPAX_EMUTRAMP
)
case $arch in
x86_64)
	karch=x86
	# make reads LLVM from the environment; x86 is always the gcc build.
	unset LLVM
	cross=()
	opts=(
		"${common[@]}"
		HYPERVISOR_GUEST PARAVIRT XEN_PV XEN_PVHVM_GUEST PCI_XEN
		XEN_PCIDEV_FRONTEND XEN_PCIDEV_BACKEND XEN_IOMMU
		HYPERV HYPERV_VMBUS HYPERV_NET PCI_HYPERV PCI_HYPERV_INTERFACE
	)
	;;
arm64)
	karch=arm64
	if [ -n "${LLVM:-}" ]; then
		cross=(LLVM=1)
	else
		cross=(CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}")
	fi
	opts=("${common[@]}")
	;;
*)
	echo "unknown arch: $arch" >&2
	exit 2
	;;
esac

mkdir -p "$objdir"
mk() { make -s ARCH="$karch" "${cross[@]}" O="$objdir" "$@"; }

mk defconfig
enable=()
for o in "${opts[@]}"; do enable+=(--enable "$o"); done
scripts/config --file "$objdir/.config" "${enable[@]}"
mk olddefconfig

# scripts/config writes whatever it is told; olddefconfig then quietly drops
# anything whose dependencies are not met. Catch that, or a Kconfig change
# could switch a downstream feature off without failing the build.
lost=()
for o in "${opts[@]}"; do
	grep -qx "CONFIG_$o=y" "$objdir/.config" || lost+=("$o")
done
if [ "${#lost[@]}" -ne 0 ]; then
	echo "kernel-build: not enabled after olddefconfig ($arch): ${lost[*]}" >&2
	exit 3
fi

mk -j"$jobs" vmlinux
echo "kernel-build: $arch vmlinux built in $objdir"
