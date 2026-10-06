O ?= build
CROSS ?= riscv64-unknown-elf-

# 启动代码与链接脚本同 hello
$(O)/periph.bin: ../hello/start.S periph.c ../hello/link.ld
	mkdir -p $(O)
	$(CROSS)gcc -march=rv32ima_zicsr -mabi=ilp32 -Os -ffreestanding -nostdlib -Wl,-T,../hello/link.ld -o $(O)/periph.elf ../hello/start.S periph.c
	$(CROSS)objcopy -O binary $(O)/periph.elf $@
