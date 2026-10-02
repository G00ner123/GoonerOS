# GoonerOS
 Hand made hobby OS. *Experimental*! Do not flash this on a Install medium etc. Test ONLY in a VM, such as QEMU. Due to Instability, Incompatibility and lack of
 Functions/Content. (If you ignore this warning and run it on real hardware anyway, that's on you, not me/Ich wiederhole: Nur in VM Testen und NICHT auf Echter hardware, das System ist Instabil, hat Wenig funktionen und mangelt an kompatiblität.) 

## Needed/Requirements:
   Needs to be compiled locally with 32-bit-GCC, and nasm or other C and assembly compilers.
   Needed to compile/boot: 32-bit-GCC, nasm, objcopy and QEMU.
   It is optimized to 32-bit-GCC, nasm, QEMU and a Linux System such as Mint or Arch (WSL on Windows might work, not tested).
   Hardware needed: BIOS x86-PC, VESA graphic-mode 0x4118, PS/2-Mouse and Keyboard and ATA/IDE. In QEMU thats not a problem.
### Programms/Packages in need:
    (Package names for Debian/Ubuntu)
     build-essential
     gcc-multilib
     binutils
     nasm
     python3
     qemu-system-x86

## Makefile Commands:
   "*make run*" compiles the code and starts qemu (if installed).
   "*make clean*" deletes all *.o, *.bin and *.elf binary-files, so that you can compile it from zero again (does not delete the os.img file).

 Comments in the code are written in German, can be ignored.
 (most of) OS and GUI is in English.
 Shell commands are in English, commands are highly inspired by Linux.
 if the keyboard Layout is German, type "loadkeys en" to set it to English.
 If Booted in a VM type "help" in the shell of the System to see all available commands.
 
<img width="1496" height="1124" alt="Bildschirmfoto_20260930_174352" src="https://github.com/user-attachments/assets/b6f55cb1-4ab6-4832-8f69-ca347e9fc943" />
<img width="1503" height="1125" alt="Bildschirmfoto_20260930_174151" src="https://github.com/user-attachments/assets/b5e6e63c-e7c6-44f0-be44-34947704d600" />
<img width="1494" height="1119" alt="grafik" src="https://github.com/user-attachments/assets/4aa91436-fff5-4196-91f8-49dbf9a7125e" />

<sub>*Please fork and modify freely — but keep it under the GPL.*<sub>

<sub>_used little bit of AI for Assembly code and design decisions_<sub>
