# "make run" Kompiliert und startet QEMU
# "make clean" Löscht alle *.bin, *elf und *.o Dateien
# "make run" compiles and starts QEMU
# "make clean" removes all of *.bin, *.elf and *.o files so that you can compile it from zero again.

ASM = nasm
CC  = gcc
LD  = ld

CFLAGS  = -m32 -ffreestanding -fno-pie -fno-stack-protector -mno-sse -mno-sse2 -mno-mmx -msoft-float -nostdlib -fno-builtin -c
LDFLAGS = -m elf_i386 -T linker.ld

QEMU      = qemu-system-i386
QEMUFLAGS = -drive file=os.img,format=raw -rtc base=localtime

# Alle C-Module des Kernels.
C_SOURCES = kernel.c paging.c gdt.c user_process.c vga.c fs.c ata.c rtc.c idt.c heap.c keyboard.c mouse.c shell.c desktop.c scheduler.c
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

user.o: user.asm
	$(ASM) -f elf32 user.asm -o user.o

usermode.o: usermode.asm
	$(ASM) -f elf32 usermode.asm -o usermode.o

# Pattern-Regel: jede beliebige xyz.c wird zu xyz.o. kernel.h/io.h als
# Abhängigkeit.
%.o: %.c kernel.h io.h
	$(CC) $(CFLAGS) $< -o $@

# Reihenfolge beim Linken: isr.o zuerst, weil dort _start
# (der echte Einsprungpunkt bei 0x100000, .text.boot) drin is.
kernel.elf: isr.o $(C_OBJECTS) interrupts.o user.o usermode.o linker.ld
	$(LD) $(LDFLAGS) -o kernel.elf isr.o $(C_OBJECTS) interrupts.o user.o usermode.o

kernel.bin: kernel.elf
	objcopy -O binary kernel.elf kernel.bin

# os.img muss auf 1 MiB aufgefüllt werden, sonst liest stage2.asm beim
# 127-Sektoren-Kernelload über das Dateiende hinaus und Boot hängt.
os.img: boot.bin stage2.bin kernel.bin build_image.py
	python3 build_image.py boot.bin stage2.bin kernel.bin os.img

run: os.img
	$(QEMU) $(QEMUFLAGS)

clean:
# os.img enthält persistente Nutzerdaten, darf beim Aufräumen nicht gelöscht werden.
	rm -f *.o *.bin *.elf
