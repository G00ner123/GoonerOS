ASM = nasm
CC  = gcc
LD  = ld

CFLAGS  = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -fno-builtin -c
LDFLAGS = -m elf_i386 -T linker.ld

QEMU      = qemu-system-i386
QEMUFLAGS = -drive file=os.img,format=raw -rtc base=localtime

# Alle C-Module des Kernels.
C_SOURCES = kernel.c vga.c fs.c ata.c rtc.c idt.c heap.c keyboard.c mouse.c shell.c desktop.c
C_OBJECTS = $(C_SOURCES:.c=.o)

.PHONY: all run clean

all: os.img

boot.bin: boot.asm
	$(ASM) -f bin boot.asm -o boot.bin

stage2.bin: stage2.asm
	$(ASM) -f bin stage2.asm -o stage2.bin

isr.o: isr.asm
	$(ASM) -f elf32 isr.asm -o isr.o

interrupts.o: interrupts.asm
	$(ASM) -f elf32 interrupts.asm -o interrupts.o

# Pattern-Regel: jede beliebige xyz.c wird zu xyz.o. kernel.h/io.h als
# Abhaengigkeit, damit bei Header-Aenderungen alles neu gebaut wird.
%.o: %.c kernel.h io.h
	$(CC) $(CFLAGS) $< -o $@

# Reihenfolge beim Linken ist Pflicht: isr.o zuerst, weil dort _start
# (der echte Einsprungpunkt bei 0x100000, .text.boot) drinsteckt.
kernel.elf: isr.o $(C_OBJECTS) interrupts.o linker.ld
	$(LD) $(LDFLAGS) -o kernel.elf isr.o $(C_OBJECTS) interrupts.o

kernel.bin: kernel.elf
	objcopy -O binary kernel.elf kernel.bin

# os.img MUSS auf 1 MiB aufgefuellt werden, sonst liest stage2.asm beim
# 127-Sektoren-Kernelload ueber das Dateiende hinaus -> Boot haengt.
os.img: boot.bin stage2.bin kernel.bin
	cat boot.bin stage2.bin kernel.bin > os.img
	truncate -s 1048576 os.img

run: os.img
	$(QEMU) $(QEMUFLAGS)

clean:
	rm -f *.o *.bin *.elf os.img
