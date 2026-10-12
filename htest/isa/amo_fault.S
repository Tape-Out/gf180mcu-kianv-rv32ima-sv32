# AMO 的写半边缺页：指令重来之前不许已经改了寄存器。
# 开 Sv32，把 DRAM 映成一张只读的大页，经 MPRV 对它做 AMO：读得到，写缺页。处理程序先核对源寄存器还是原值，
# 再把页改成可写、返回，指令重来。rd 与 rs2、rd 与 rs1 是同一个寄存器的都测：GCC 的原子交换是前一种，
# glibc 的解锁用它，写时复制的页上解锁重来时把旧值写回去，锁就放不掉。

#include "riscv_test.h"
#include "test_macros.h"

#define OLD 0x11111111

# 回到只读，陷入次数清零
.macro readonly
  li t0, MSTATUS_MPRV
  csrc mstatus, t0
  lw t0, page_table_1
  andi t0, t0, ~(PTE_W | PTE_D)
  sw t0, page_table_1, t1
  sfence.vma
  li t0, MSTATUS_MPRV
  csrs mstatus, t0
  li s1, 0
.endm

RVTEST_RV32M
RVTEST_CODE_BEGIN

  la s0, word
  li t0, OLD
  sw t0, 0(s0)
  sw t0, 4(s0)
  sw t0, 8(s0)
  sw t0, 12(s0)
  li t0, DRAM_BASE
  sub s2, s0, t0

  li a0, SATP32_MODE
  la a1, page_table_1
  srl a1, a1, RISCV_PGSHIFT
  or a1, a1, a0
  csrw satp, a1
  sfence.vma

  # MPRV、MPP=S、SUM：取数与存数按 S 态走页表。陷入返回之后 MPP 落到 U，页带 U 位，照样读得到
  li a1, ((MSTATUS_MPP & ~(MSTATUS_MPP<<1)) * PRV_S) | MSTATUS_MPRV | MSTATUS_SUM
  csrs mstatus, a1
  li s1, 0

  # rd 与 rs2 同一个寄存器的 amoswap
  li TESTNUM, 2
  li a5, 0x55555555
  amoswap.w a5, a5, (s2)
  li t0, OLD
  bne a5, t0, die
  lw t1, 0(s2)
  li t0, 0x55555555
  bne t1, t0, die
  li t0, 1
  bne s1, t0, die

  # rd 与 rs1 同一个寄存器：结果盖掉的是地址
  li TESTNUM, 3
  readonly
  addi a4, s2, 4
  li a3, 0x66666666
  amoswap.w a4, a3, (a4)
  li t0, OLD
  bne a4, t0, die
  lw t1, 4(s2)
  li t0, 0x66666666
  bne t1, t0, die
  li t0, 1
  bne s1, t0, die

  # rd 与 rs2 同一个寄存器的 amoadd
  li TESTNUM, 4
  readonly
  addi a4, s2, 8
  li a5, 7
  amoadd.w a5, a5, (a4)
  li t0, OLD
  bne a5, t0, die
  lw t1, 8(s2)
  li t0, OLD + 7
  bne t1, t0, die
  li t0, 1
  bne s1, t0, die

  # sc.w 缺页：重来时预留已经没了，可以失败，但成败要与内存对得上
  li TESTNUM, 5
  readonly
  addi a4, s2, 12
  lr.w t2, (a4)
  li a5, 0x77777777
  sc.w a5, a5, (a4)
  lw t1, 12(s2)
  li t0, 0x77777777
  beqz a5, 1f
  li t0, OLD
1:
  bne t1, t0, die
  li t0, 1
  bne s1, t0, die

  RVTEST_PASS

  .align 2
  .global mtvec_handler
mtvec_handler:
  csrr t0, mcause
  add t0, t0, -CAUSE_STORE_PAGE_FAULT
  bnez t0, die

  # 缺页的那条指令不许已经动了自己的源寄存器
  li t1, 2
  bne TESTNUM, t1, 1f
  li t1, 0x55555555
  bne a5, t1, die
  j fix
1:
  li t1, 3
  bne TESTNUM, t1, 1f
  addi t1, s2, 4
  bne a4, t1, die
  j fix
1:
  li t1, 4
  bne TESTNUM, t1, 1f
  li t1, 7
  bne a5, t1, die
  j fix
1:
  li t1, 5
  bne TESTNUM, t1, die
  li t1, 0x77777777
  bne a5, t1, die

fix:
  addi s1, s1, 1
  lw t0, page_table_1
  ori t0, t0, PTE_W | PTE_D
  sw t0, page_table_1, t1
  sfence.vma
  mret

die:
  RVTEST_FAIL

RVTEST_CODE_END

  .data
RVTEST_DATA_BEGIN

  TEST_DATA

word: .word 0, 0, 0, 0

.align 12
page_table_1: .word (DRAM_BASE/RISCV_PGSIZE << PTE_PPN_SHIFT) | PTE_V | PTE_U | PTE_R | PTE_X | PTE_A

RVTEST_DATA_END
