# 程序就地在 Flash 里跑，没有 .data 与 .bss；栈放在 SDRAM 的第一兆
.section .text.start
.global _start
_start:
    li sp, 0x80100000
    call main
1:  j 1b
