#!/bin/bash

ARGS=$1
case $ARGS in
     'M' | 'm')
		DK=M;;
     *)
		DK=N;;
esac

rm -f /home/ryanandri/android/android_kernel_motoe/clarity-$DK-condor/boot/zImage-dtb
rm -f /home/ryanandri/android/android_kernel_motoe/clarity-$DK-condor/system/lib/modules/*.ko
rm -f /home/ryanandri/android/android_kernel_motoe/arch/arm/boot/zImage

mv /home/ryanandri/android/android_kernel_motoe/arch/arm/boot/zImage-dtb /home/ryanandri/android/android_kernel_motoe/clarity-$DK-condor/boot

# get modules into one place
find -name "*.ko" -exec cp {} /home/ryanandri/android/android_kernel_motoe/clarity-$DK-condor/system/lib/modules \;
sleep 2

if [ $DK == 'M' ]; then
mv /home/ryanandri/android/android_kernel_motoe/clarity-$DK-condor/system/lib/modules/wlan.ko /home/ryanandri/android/android_kernel_motoe/clarity-$DK-condor/system/lib/modules/pronto/pronto_wlan.ko
fi

cd /home/ryanandri/android/android_kernel_motoe/clarity-$DK-condor

DATE=`date +%d-%m-%Y`;
zip -r /home/ryanandri/android/release/Clarity-$DK-$DATE-lite.zip *

