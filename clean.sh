#!/bin/bash

rm -f /home/ryanandri/android/android_kernel_motoe/arch/arm/mach-msm/smd_rpc_sym.c
make clean && make mrproper
rm -f /home/ryanandri/android/android_kernel_motoe/clarity-N-condor/boot/zImage-dtb
rm -f /home/ryanandri/android/android_kernel_motoe/clarity-M-condor/boot/zImage-dtb
rm -f /home/ryanandri/android/android_kernel_motoe/clarity-N-condor/system/lib/modules/*.ko
rm -f /home/ryanandri/android/android_kernel_motoe/clarity-M-condor/system/lib/modules/*.ko

