cmd_/root/dcsc/dcsc.ko := ld -r -m elf_x86_64 -T ./scripts/module-common.lds --build-id  -o /root/dcsc/dcsc.ko /root/dcsc/dcsc.o /root/dcsc/dcsc.mod.o
