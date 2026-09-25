# GoonerOS
# Hand made hobby OS. Experimental! Do not flash this on a Install medium etc. Test only in a VM such as QEMU.

Needed/Requirements:
  # Needs to be compiled locally with 32-bit-GCC, and nasm or other C and assembly compilers.
  # Needed to compile/boot: 32-bit-GCC, nasm, objcopy and QEMU (newest version of all of them).
  # It is optimized to 32-bit-GCC, nasm, QEMU and a Linux System such as Mint or Arch (WSL on Windows might work, not testet).
  # Hardware needed: BIOS x86-PC, VESA graphic-mode 0x4118, PS/2-Mouse and Keyboard and ATA/IDE. In QEMU thats not a problem.

Makefile Commands:
  # "make run" compiles the code and starts qemu (if installed).
  # "make" just compiles it.
  # "make clean" deletes all *.o, *.bin and *.elf binary-files, so that you can compile it from zero again (does not delete the os.img file).

# Comments in the code are written in German, can be ignored.
# (most of) OS and GUI is in English.
# Shell commands are in English, commands are highly inspired by Linux.
# if the keyboard Layout is German, type "loadkeys en" to set it to English.
# If Booted in a VM type "help" in the shell of the System to see all available commands.
