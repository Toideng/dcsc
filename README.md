How to run:

```
yum update
yum install kernel-devel
yum install gcc
reboot
```

then:

```
chmod u+x dcsc_load dcsc_add_device dcsc_unload
make
```

For non-interactive usage:
```
./dcsc_load fixed
cat /sys/bus/testbus/devices/dcsca/size
cat /sys/bus/testbus/devices/dcsca/access
echo -n <newsize> > /sys/bus/testbus/devices/dcsca/size
echo -n <newaccess> > /sys/bus/testbus/devices/dcsca/access
dd if=... of=/dev/dcsca
./dcsc_unload
```

For interactive usage:
```
./dcsc_load interactive
./dcsc_add_device <devicename> <devicesize>
cat /sys/bus/testbus/devices/<devicename>/size
cat /sys/bus/testbus/devices/<devicename>/access
echo <newsize> > /sys/bus/testbus/devices/<devicename>/size
echo <newaccess> > /sys/bus/testbus/devices/<devicename>/access
dd if=... of=/dev/<devicename>
./dcsc_unload
```
