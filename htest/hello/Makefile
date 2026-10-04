O ?= build
CROSS ?= riscv64-unknown-elf-

$(O)/hello.bin: start.S hello.c link.ld
	mkdir -p $(O)
	$(CROSS)gcc -march=rv32ima_zicsr -mabi=ilp32 -Os -ffreestanding -nostdlib -Wl,-T,link.ld -o $(O)/hello.elf start.S hello.c
	$(CROSS)objcopy -O binary $(O)/hello.elf $@
