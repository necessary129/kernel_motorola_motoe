#!/sbin/sh

# Thanks to and all credits goes to pinkflozd & kobridge @xda-developer
# edited for run to general devices by Ryan Andri a.k.a Rainforce279

mkdir /tmp/ramdisk
cd /tmp/ramdisk
gunzip -c ../boot.img-ramdisk.gz | cpio -i

found=$(find init.qcom.rc -type f | xargs grep -oh "# init run on boot");
if [ "$found" != '# init run on boot' ]; then
	echo "" >> init.qcom.rc
	echo "# init run on boot" >> init.qcom.rc
	echo "service sysinit /system/bin/logwrapper /system/xbin/busybox run-parts /system/etc/init.d" >> init.qcom.rc
	echo "	class main" >> init.qcom.rc
	echo "	user root" >> init.qcom.rc
	echo "	group shell" >> init.qcom.rc
	echo "	oneshot" >> init.qcom.rc
	echo "" >> init.qcom.rc
fi

find . | cpio -o -H newc | gzip > ../newramdisk.cpio.gz
cd /
