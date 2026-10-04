#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))
#define LSR (*(volatile uint8_t *)0x10000005)

static void putch(char c) {
  while (!(LSR & 0x60)) {}
  REG(0x10000000) = c;
}

static void say(const char *s) {
  while (*s) putch(*s++);
}

static void hex(uint32_t v) {
  for (int i = 28; i >= 0; i -= 4) putch("0123456789abcdef"[v >> i & 15]);
}

static uint32_t want(uint32_t i) { return i * 0x9e3779b1u ^ 0xa5a5a5a5u; }

void main(void) {
  /* 分频寄存器复位后是 1。主频寄存器是 8.8 定点的兆赫，乘 15625/4 得赫兹 */
  uint32_t hz = (REG(0x10000014) & 0xffff) * 15625 / 4;
  REG(0x1000000c) = hz / 115200;
  say("KianV hello\n");

  /* 步长取素数，1024 个字散在 16 MiB 里，行、列、bank 都换到 */
  volatile uint32_t *m = (volatile uint32_t *)0x80000000;
  uint32_t bad = 0;
  for (uint32_t i = 0; i < 1024; i++) m[i * 4099] = want(i);
  for (uint32_t i = 0; i < 1024; i++) bad += m[i * 4099] != want(i);

  /* 字节与半字写只动自己那几位 */
  volatile uint8_t *b = (volatile uint8_t *)0x81fffff8;
  REG(0x81fffff8) = 0x11223344;
  REG(0x81fffffc) = 0x55667788;
  b[1] = 0xaa;
  *(volatile uint16_t *)(b + 6) = 0xbbcc;
  bad += REG(0x81fffff8) != 0x1122aa44;
  bad += REG(0x81fffffc) != 0xbbcc7788;

  say("sdram ");
  say(bad ? "BAD " : "ok ");
  hex(bad);
  say("\nmem ");
  hex(REG(0x10000018));
  say("\ndone\n");
}
