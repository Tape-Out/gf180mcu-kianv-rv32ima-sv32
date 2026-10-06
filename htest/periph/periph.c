/* 片上外设逐个点一遍：GPIO、两路 SPI、CLINT 的计时器中断与软件中断、PLIC（串口那一路）、重启。
 * 测试台在两路 SPI 上各挂一个回声从设备（每个字节回上一个字节的反码，片选一抬就忘），GPIO 脚上有上拉。
 */
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))
#define LSR (*(volatile uint8_t *)0x10000005)
#define CSR_R(n) ({ uint32_t v_; __asm__ volatile("csrr %0, " #n : "=r"(v_)); v_; })
#define CSR_W(n, v) __asm__ volatile("csrw " #n ", %0" : : "r"((uint32_t)(v)))
#define CSR_S(n, v) __asm__ volatile("csrs " #n ", %0" : : "r"((uint32_t)(v)))

/* 程序就地在 Flash 里跑，没有 .data：中断里要记的几个数放在 SDRAM 里固定的地方 */
#define STATE ((volatile uint32_t *)0x80000100)
enum { CAUSE, COUNT, CLAIM, MARK };
#define REBOOTED 0xb007b007u

#define GPIO 0x10000700u
#define SPI0 0x10500000u
#define SPI1 0x10500100u
#define CLINT 0x02000000u
#define PLIC 0x0c000000u
#define UART_IRQ 10

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

static void verdict(const char *what, uint32_t bad, uint32_t detail) {
  say(what);
  say(bad ? " BAD " : " ok ");
  hex(detail);
  putch('\n');
}

__attribute__((interrupt("machine"), aligned(4))) static void trap(void) {
  uint32_t c = CSR_R(mcause);
  STATE[CAUSE] = c;
  STATE[COUNT]++;
  if (c == 0x80000007) {
    REG(CLINT + 0x4004) = 0xffffffff;
    REG(CLINT + 0x4000) = 0xffffffff;
  } else if (c == 0x80000003) {
    /* 清掉 msip 就该不再进来：mip.MSIP 跟着它走。上游原样做不到，靠 patch/msip.patch */
    REG(CLINT) = 0;
  } else if (c == 0x8000000b) {
    /* 串口在发字期间那根线一直是高的：先关使能再交还，否则刚交还又挂起 */
    REG(PLIC + 0x2000) = 0;
    STATE[CLAIM] = REG(PLIC + 0x200004);
    REG(PLIC + 0x200004) = STATE[CLAIM];
  } else {
    say("trap ");
    hex(c);
    putch(' ');
    hex(CSR_R(mepc));
    putch('\n');
    for (;;) {}
  }
}

/* 等中断处理程序把次数加上去，最多等这么多微秒 */
static uint32_t waited(uint32_t n, uint32_t us) {
  uint32_t t0 = REG(CLINT + 0xbff8);
  while (STATE[COUNT] == n && REG(CLINT + 0xbff8) - t0 < us) {}
  return STATE[COUNT] != n;
}

static uint8_t xfer(uint32_t spi, uint8_t v) {
  REG(spi + 4) = v;
  while (REG(spi) >> 31) {}
  return REG(spi + 4);
}

/* 回声从设备：头一个字节回 ff，之后回上一个字节的反码；片选重新拉下之后又从 ff 起 */
static uint32_t spi_test(uint32_t spi) {
  uint32_t got = 0, bad = 0;
  REG(spi) = 1;
  bad |= (REG(spi) & 1) != 0;
  got = xfer(spi, 0xa5);
  got = got << 8 | xfer(spi, 0x3c);
  got = got << 8 | xfer(spi, 0x00);
  REG(spi) = 0;
  bad |= (REG(spi) & 1) != 1;
  REG(spi) = 1;
  got = got << 8 | xfer(spi, 0x81);
  REG(spi) = 0;
  return bad << 31 | (got ^ 0xff5ac3ff);
}

void main(void) {
  uint32_t hz = (REG(0x10000014) & 0xffff) * 15625 / 4;
  /* 分频一的高 16 位是 SPI0 的分频；分频二的高 16 位是 SPI1 的，低 16 位是计时器的，要留在 1 */
  REG(0x1000000c) = 4 << 16 | hz / 115200;
  REG(0x10000010) = 2 << 16 | 1;
  say("KianV periph\n");
  if (STATE[MARK] == REBOOTED) {
    STATE[MARK] = 0;
    say("rebooted\ndone\n");
    return;
  }
  STATE[CAUSE] = STATE[COUNT] = STATE[CLAIM] = 0;

  /* 方向与输出写在第 9 位，读回在第 0 位，这是上游的写法 */
  uint32_t g = 0;
  REG(GPIO) = 1 << 9;
  REG(GPIO + 4) = 1 << 9;
  g |= (REG(GPIO + 8) & 1) != 1;
  REG(GPIO + 4) = 0;
  g |= ((REG(GPIO + 8) & 1) != 0) << 1;
  g |= (REG(GPIO) != 1 || REG(GPIO + 4) != 0) << 2;
  REG(GPIO) = 0;
  g |= ((REG(GPIO + 8) & 1) != 1) << 3;
  verdict("gpio", g, g);

  uint32_t s = spi_test(SPI0);
  verdict("spi0", s, s);
  s = spi_test(SPI1);
  verdict("spi1", s, s);

  CSR_W(mtvec, (uint32_t)trap);
  CSR_S(mstatus, 8);

  /* 计时器：mtime 每微秒走一格，与 time 寄存器是同一个数；到点进中断 */
  uint32_t t = REG(CLINT + 0xbff8), bad = CSR_R(time) - t > 50;
  CSR_S(mie, 1 << 7);
  REG(CLINT + 0x4004) = 0;
  REG(CLINT + 0x4000) = t + 300;
  bad |= !waited(0, 5000) << 1;
  bad |= (STATE[CAUSE] != 0x80000007) << 2;
  bad |= (REG(CLINT + 0xbff8) - t < 300) << 3;
  verdict("timer", bad, STATE[CAUSE]);

  CSR_S(mie, 1 << 3);
  REG(CLINT) = 1;
  bad = !waited(1, 5000);
  bad |= (STATE[CAUSE] != 0x80000003 || REG(CLINT) != 0) << 1;
  bad |= (CSR_R(mip) >> 3 & 1) << 2;
  verdict("soft", bad, STATE[CAUSE]);

  /* PLIC：使能串口那一路，发一个字，发的期间线是高的，进外部中断，领到的号是 10 */
  CSR_S(mie, 1 << 11);
  REG(PLIC + 0x2000) = 1 << UART_IRQ;
  putch('.');
  bad = !waited(2, 5000);
  bad |= (STATE[CAUSE] != 0x8000000b) << 1;
  bad |= (STATE[CLAIM] != UART_IRQ) << 2;
  putch('\n');
  verdict("plic", bad, STATE[CLAIM]);

  STATE[MARK] = REBOOTED;
  say("reboot\n");
  while (!(LSR & 0x40)) {}
  REG(0x11100000) = 0x7777;
  for (;;) {}
}
