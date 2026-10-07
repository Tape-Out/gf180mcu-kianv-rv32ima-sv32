// 整颗 KianV SoC 的测试台。片外的 SDRAM、SPI Flash 与串口按引脚电平建模，接在一条 54 位的焊盘总线上，
// 位次照上游 chip_core.sv；上游源码与流片交付的那份展平文件各包一层 tb，用的是同一份测试台。
//
//   +flash=<文件>@<偏移>   往 Flash 里放一段，可多次给
//   +sdram=<文件>@<偏移>   直接放进 SDRAM，不经引导程序搬（riscv-tests 用，调试也用）
//   +tohost=<地址>         盯 riscv-tests 的 tohost：写 1 算过，写别的数算不过；给了它就不必给脚本
//   +script=<文件>         逐行 expect <文本> / send <文本> / save <文件>，全部走完算过；
//                          reject <文本> 不占次序，串口上一出现这段字就算不过，不必等到 +max
//   +restore=<文件>        从断点接着跑：save 存下的，脚本从头走；+stop 存下的，同一份脚本从停下的那一步接着走
//   +stop=<n>@<文件>       这一次跑满 n 个周期就把断点存进文件、算过退出：一个作业放不下的启动分几个作业跑完；
//                          写成 <n>s 是跑满 n 秒，托管机快慢不一，按时间分段才不会超时
//   +spiecho               两路 SPI 上各挂一个回声从设备；不给时 MISO 是高的，像没插卡、没接网卡
//   +sd=<镜像>             SPI0 上插一张 SD 卡，内容是这个文件（补齐到 512 KiB 的整数倍）
//   +sdout=<文件>          跑完把卡里的内容写出来
//   +w5500                 SPI1 上接一颗 W5500，网线那头是一台会应 ARP 与 ping 的主机 10.0.0.1
//   +nicpipe=<目录>        同上，网线接到另一个仿真上（比如交换机与路由器的联合仿真），帧经这个目录里的两个文件来往
//   +gpu                   SPI1 上接一整颗 to2610-gpu（它的管理口），它的 done 接 GPIO；仿真器要编进它（sim.sh 的 SIM_GPU）
//   +uartdiv=<n>           串口每位占几个时钟
//   +pace=<n>              往芯片发的相邻两个字之间空几个时钟
//   +max=<n>               最多跑几个时钟周期，到了还没走完算不过
//   +beat=<n>              每 n 个周期往标准错误报一次进度：小时级的仿真要看得出它还活着
//
// 断点是给 Linux 用的：起到 shell 要仿一个多小时，存一次，之后调命令从断点起。
// 片上的串口接收缓冲只有 16 个字，内核又是按时钟节拍去取的，一口气发一整行会冲掉，所以要 +pace。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "Vtb.h"
#include "verilated.h"
#include "verilated_save.h"
#ifdef WITH_GPU
#include "Vgpu.h"
#endif

namespace pad {
enum : int {
  UART_RX = 0, SPI0_MISO = 1, FLASH_MISO = 2, SPI1_MISO = 3,
  UART_TX = 4, SPI0_CSN = 5, SPI0_SCLK = 6, SPI0_MOSI = 7, FLASH_CSN = 8, FLASH_SCLK = 9, FLASH_MOSI = 10,
  SPI1_CSN = 11, SPI1_SCLK = 12, SPI1_MOSI = 13, GPIO = 53,
  SD_CKE = 15, SD_DQM = 16, SD_ADDR = 18, SD_BA = 31,
  SD_CSN = 33, SD_WEN = 34, SD_RASN = 35, SD_CASN = 36, SD_DQ = 37,
};
}

static inline uint64_t bits(uint64_t v, int lo, int n) { return (v >> lo) & ((1ull << n) - 1); }

// 断点文件里的标量与数组都按内存里的样子原样读写，只在同一台机器上用
struct Ckpt {
  FILE *f;
  bool out;
  void raw(void *p, size_t n) {
    if ((out ? fwrite(p, 1, n, f) : fread(p, 1, n, f)) != n) { perror("断点"); exit(2); }
  }
  template <class T> void operator()(T &v) { raw(&v, sizeof v); }
  template <class T> void seq(std::deque<T> &q) {
    size_t n = q.size();
    (*this)(n);
    if (!out) q.resize(n);
    for (auto &x : q) (*this)(x);
  }
  void str(std::string &s) {
    size_t n = s.size();
    (*this)(n);
    if (!out) s.resize(n);
    raw(s.data(), n);
  }
};

// MT48LC16M16A2：4 个 bank、13 位行、9 位列、16 位数据。只建功能，不查时序参数
struct Sdram {
  std::vector<uint16_t> mem = std::vector<uint16_t>(1u << 24);
  uint32_t row[4] = {};
  int cl = 2, bl = 1;
  long edge = 0;
  struct Out { long at; uint16_t v; };
  std::deque<Out> rd;
  int wleft = 0, wi = 0;
  uint32_t wbase = 0, wcol = 0;
  uint16_t dq = 0;
  long reads = 0, writes = 0;
  // riscv-tests 往 tohost 写结果：盯它的低半个字落在模型里的哪一格
  long watch = -1;
  bool hit = false;
  uint16_t host = 0;

  // 控制器把地址拆成 bank = addr[22:21]、行 = {addr[24:23], addr[20:10]}、列 = addr[9:1]，模型按 bank、行、列排。
  // 给的是 SDRAM 里的字节偏移，回的是它在模型里的字节偏移
  static uint32_t wire(uint32_t a) {
    uint32_t blk = a >> 10, bank = blk >> 11 & 3, row = (blk >> 13 & 3) << 11 | (blk & 0x7ff);
    return (bank << 13 | row) << 10 | (a & 0x3ff);
  }

  void ckpt(Ckpt &c) {
    c.raw(mem.data(), mem.size() * sizeof mem[0]);
    c(row), c(cl), c(bl), c(edge), c.seq(rd), c(wleft), c(wi), c(wbase), c(wcol), c(dq), c(reads), c(writes);
  }
  uint32_t at(uint32_t base, uint32_t col, int i) const {
    return base | ((col & ~(uint32_t)(bl - 1)) | ((col + i) & (bl - 1)));
  }
  void put(uint64_t out) {
    uint16_t d = bits(out, pad::SD_DQ, 16), m = bits(out, pad::SD_DQM, 2);
    uint16_t &w = mem[at(wbase, wcol, wi)];
    if (!(m & 1)) w = (w & 0xff00) | (d & 0x00ff);
    if (!(m & 2)) w = (w & 0x00ff) | (d & 0xff00);
    if ((long)at(wbase, wcol, wi) == watch && !(m & 1)) hit = true, host = w;
    ++wi, --wleft, ++writes;
  }
  // sdram_clk 的上升沿调一次，out 是这一沿上芯片驱动的引脚
  void clk(uint64_t out) {
    uint32_t a = bits(out, pad::SD_ADDR, 13), ba = bits(out, pad::SD_BA, 2);
    int cmd = bits(out, pad::SD_RASN, 1) << 2 | bits(out, pad::SD_CASN, 1) << 1 | bits(out, pad::SD_WEN, 1);
    if (bits(out, pad::SD_CSN, 1)) cmd = 7;
    switch (cmd) {
      case 0: bl = 1 << (a & 7); cl = (a >> 4) & 7; break;
      case 3: row[ba] = a; break;
      case 4:
        wbase = ba << 22 | row[ba] << 9, wcol = a & 0x1ff, wleft = bl, wi = 0;
        put(out);
        break;
      case 5: {
        uint32_t base = ba << 22 | row[ba] << 9, col = a & 0x1ff;
        while (!rd.empty() && rd.back().at >= edge + cl - 1) rd.pop_back();
        for (int i = 0; i < bl; ++i) rd.push_back({edge + cl - 1 + i, mem[at(base, col, i)]});
        wleft = 0, ++reads;
        break;
      }
      case 6:
        wleft = 0;
        while (!rd.empty() && rd.back().at >= edge + cl - 1) rd.pop_back();
        break;
      case 7: if (wleft) put(out); break;
      default: wleft = 0; break;
    }
    if (!rd.empty() && rd.front().at == edge) dq = rd.front().v, rd.pop_front();
    ++edge;
  }
};

// 只认 0x03 读：命令与 24 位地址之后，片选不抬就一直往后吐
struct Flash {
  std::vector<uint8_t> mem = std::vector<uint8_t>(16u << 20, 0xff);
  bool sclk = false, miso = true;
  int n = 0, bit = 7;
  uint32_t sh = 0, addr = 0;

  // 内容不进断点：恢复时照样用 +flash 给
  void ckpt(Ckpt &c) { c(sclk), c(miso), c(n), c(bit), c(sh), c(addr); }
  void step(bool cs, bool ck, bool mosi) {
    if (cs) { n = 0, sclk = ck; return; }
    if (!sclk && ck) {
      if (n < 32) {
        sh = sh << 1 | mosi;
        if (++n == 32) addr = sh & 0xffffff, bit = 7;
      } else if (bit-- == 0) {
        bit = 7, addr = (addr + 1) & 0xffffff;
      }
    } else if (sclk && !ck && n == 32) {
      miso = mem[addr] >> bit & 1;
    }
    sclk = ck;
  }
};

// 回声从设备，SPI 模式 0：片选期间头一个字节回 ff，之后每个字节回上一个字节的反码；片选一抬就忘。
// 回得对，说明片选、时钟、MOSI、MISO 四根线与字节的位次都对
struct Echo {
  bool sclk = false, miso = true;
  int n = 0;
  uint8_t sh = 0, out = 0xff;

  void ckpt(Ckpt &c) { c(sclk), c(miso), c(n), c(sh), c(out); }
  void step(bool cs, bool ck, bool mosi) {
    if (cs) { n = 0, out = 0xff, miso = true, sclk = ck; return; }
    if (!sclk && ck) {
      sh = sh << 1 | mosi;
      if (++n == 8) n = 0, out = ~sh;
    } else if (sclk && !ck) {
      miso = out >> (7 - n) & 1;
    }
    sclk = ck;
  }
};

// SPI 模式的 SD 卡（SDHC，按块寻址），内容是 +sd 给的镜像。照 SD Physical Layer Simplified Specification
// 第 7 章，认 Linux 的 mmc_spi 用到的那些命令：上电识别（CMD0、8、55 加 ACMD41、58、59），读寄存器
// （CMD9、10、13，ACMD13、51，CMD6），读写单块与多块（CMD17、18、12、24、25，ACMD23），擦除（CMD32、33、38）。
// 发出去的数据带 CRC16；主机发来的 CRC 不查。片选抬起只让出 MISO、重新对齐字节，卡里排着的应答不丢。
// 卡的状态不进断点：卡是恢复之后才「插」上的
struct SdCard {
  std::vector<uint8_t> mem;
  bool sclk = false, miso = true, sel = false;
  int nbit = 0;
  uint8_t sh = 0, cur = 0xff;
  std::deque<uint8_t> out;
  uint8_t cmd[6] = {};
  int ncmd = 0;
  bool idle = true, app = false;
  int tries = 0;
  // 写：0 不在写，1 等单块的令牌，2 等多块的令牌，3 在收一块
  int wstate = 0;
  uint32_t wblk = 0;
  int wn = 0;
  uint8_t wbuf[514] = {};
  bool multi = false, rmulti = false;
  uint32_t rblk = 0;
  long reads = 0, writes = 0;

  uint32_t blocks() const { return mem.size() / 512; }
  static uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; ++i) {
      c ^= p[i] << 8;
      for (int k = 0; k < 8; ++k) c = c & 0x8000 ? c << 1 ^ 0x1021 : c << 1;
    }
    return c;
  }
  static uint8_t crc7(const uint8_t *p, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; ++i)
      for (int k = 7; k >= 0; --k) {
        c <<= 1;
        if ((p[i] >> k ^ c >> 7) & 1) c ^= 0x09;
      }
    return c << 1 | 1;
  }
  // 一个数据块：起始令牌、内容、CRC16
  void block(const uint8_t *p, size_t n) {
    out.push_back(0xfe);
    out.insert(out.end(), p, p + n);
    uint16_t c = crc16(p, n);
    out.push_back(c >> 8), out.push_back(c & 0xff);
  }
  void reg(std::initializer_list<uint8_t> v) {
    uint8_t r[16] = {};
    std::copy(v.begin(), v.end(), r);
    r[15] = crc7(r, 15);
    block(r, 16);
  }
  void exec() {
    int c = cmd[0] & 0x3f;
    uint32_t arg = (uint32_t)cmd[1] << 24 | cmd[2] << 16 | cmd[3] << 8 | cmd[4];
    bool acmd = app;
    uint8_t r1 = idle;
    app = false, rmulti = false;
    // 应答之前隔一个字节；多块读到一半来的命令把没发完的那一块作废
    out.clear();
    out.push_back(0xff);
    if (acmd) {
      static const uint8_t zero[64] = {};
      // SCR：SD 2.00，一线与四线
      static const uint8_t scr[8] = {0x02, 0x35, 0x80};
      switch (c) {
        case 41:
          if (++tries >= 2) idle = false;
          out.push_back(idle);
          break;
        case 13: out.push_back(r1), out.push_back(0), out.push_back(0xff), block(zero, 64); break;
        case 51: out.push_back(r1), out.push_back(0xff), block(scr, 8); break;
        case 23: case 42: out.push_back(r1); break;
        default: out.push_back(r1 | 0x04);
      }
      return;
    }
    switch (c) {
      case 0: idle = true, tries = 0, wstate = 0, out.push_back(0x01); break;
      case 8: out.push_back(r1), out.push_back(0), out.push_back(0), out.push_back(cmd[3] & 0x0f), out.push_back(cmd[4]); break;
      case 9: {
        // CSD 2.0：容量 =（C_SIZE + 1）× 512 KiB
        uint32_t cs = mem.size() / (512 * 1024) - 1;
        out.push_back(r1), out.push_back(0xff);
        reg({0x40, 0x0e, 0x00, 0x32, 0x5b, 0x59, 0x00, (uint8_t)(cs >> 16 & 0x3f), (uint8_t)(cs >> 8), (uint8_t)cs,
             0x7f, 0x80, 0x0a, 0x40, 0x00});
        break;
      }
      case 10:
        out.push_back(r1), out.push_back(0xff);
        reg({0x74, 'T', 'O', '2', '6', '1', '0', 'S', 0x10, 0x26, 0x10, 0x00, 0x01, 0x01, 0xaa});
        break;
      case 12: out.push_back(r1); break;
      case 13: out.push_back(r1), out.push_back(0); break;
      case 16: out.push_back(arg == 512 ? r1 : r1 | 0x40); break;
      case 17:
        if (arg >= blocks()) { out.push_back(r1 | 0x40); break; }
        out.push_back(r1), out.push_back(0xff), block(&mem[(size_t)arg * 512], 512), ++reads;
        break;
      case 18:
        if (arg >= blocks()) { out.push_back(r1 | 0x40); break; }
        out.push_back(r1), rmulti = true, rblk = arg;
        break;
      case 24: case 25:
        if (arg >= blocks()) { out.push_back(r1 | 0x40); break; }
        out.push_back(r1), wstate = c == 24 ? 1 : 2, wblk = arg;
        break;
      case 32: case 33: case 59: out.push_back(r1); break;
      case 38: out.push_back(r1), out.push_back(0x00); break;
      case 55: app = true, out.push_back(r1); break;
      case 58: out.push_back(r1), out.push_back(idle ? 0x00 : 0xc0), out.push_back(0xff), out.push_back(0x80), out.push_back(0x00); break;
      case 6: {
        static const uint8_t st[64] = {};
        out.push_back(r1), out.push_back(0xff), block(st, 64);
        break;
      }
      default: out.push_back(r1 | 0x04);
    }
  }
  void byte(uint8_t b) {
    if (wstate == 3) {
      wbuf[wn++] = b;
      if (wn < 514) return;
      if (wblk < blocks()) memcpy(&mem[(size_t)wblk * 512], wbuf, 512), ++writes;
      ++wblk;
      // 收下了，再忙两个字节
      out.push_back(0x05), out.push_back(0x00), out.push_back(0x00);
      wstate = multi ? 2 : 0;
      return;
    }
    if (ncmd == 0 && wstate == 1 && b == 0xfe) { wstate = 3, multi = false, wn = 0; return; }
    if (ncmd == 0 && wstate == 2 && b == 0xfc) { wstate = 3, multi = true, wn = 0; return; }
    if (ncmd == 0 && wstate == 2 && b == 0xfd) { wstate = 0, out.push_back(0xff), out.push_back(0x00); return; }
    if (ncmd == 0 && (b & 0xc0) != 0x40) return;
    cmd[ncmd++] = b;
    if (ncmd == 6) ncmd = 0, exec();
  }
  void step(bool cs, bool ck, bool mosi) {
    if (cs) { nbit = 0, miso = true, sel = false, sclk = ck; return; }
    if (!sel) sel = true, miso = cur >> 7;
    if (!sclk && ck) {
      sh = sh << 1 | mosi;
      if (++nbit == 8) {
        nbit = 0;
        byte(sh);
        if (out.empty() && rmulti && rblk < blocks())
          out.push_back(0xff), block(&mem[(size_t)rblk * 512], 512), ++rblk, ++reads;
        cur = 0xff;
        if (!out.empty()) cur = out.front(), out.pop_front();
      }
    } else if (sclk && !ck) {
      miso = cur >> (7 - nbit) & 1;
    }
    sclk = ck;
  }
};

// SPI 网卡 W5500，只建 Linux 的 w5100 驱动用到的那一截：通用寄存器（复位、MAC、RTR、版本号）与 0 号套接字的
// MACRAW 模式（OPEN、CLOSE、SEND、RECV，发送与接收缓冲各 16 KB）。SPI 帧照数据手册 2.2：两字节地址、一字节控制
// （块号 7:3、写 2、OM 1:0 只认 00 变长），之后是数据，地址在块内自增。
// 网线那头是一台主机 10.0.0.1：应 ARP，回 ping，当 DHCP 服务器。网卡状态不进断点，与 SD 卡一样是恢复之后才「插」上的
struct W5500 {
  uint8_t common[0x40] = {}, sock[0x30] = {};
  std::vector<uint8_t> tx = std::vector<uint8_t>(16384), rx = std::vector<uint8_t>(16384);
  bool sclk = false, miso = true, sel = false;
  int nbit = 0, n = 0;
  uint8_t sh = 0, cur = 0xff, ctl = 0;
  uint16_t addr = 0;
  long sent = 0, got = 0, arps = 0, pings = 0;
  static constexpr uint8_t host_mac[6] = {0x02, 0x26, 0x10, 0x00, 0x00, 0x01};
  static constexpr uint8_t host_ip[4] = {10, 0, 0, 1};

  W5500() { reset(); }
  void reset() {
    memset(common, 0, sizeof common);
    memset(sock, 0, sizeof sock);
    common[0x19] = 0x07, common[0x1a] = 0xd0, common[0x1b] = 8, common[0x39] = 0x04, common[0x2e] = 0xbf;
    sock[0x1e] = sock[0x1f] = 2;
  }
  uint16_t r16(int a) const { return sock[a] << 8 | sock[a + 1]; }
  void w16(int a, uint16_t v) { sock[a] = v >> 8, sock[a + 1] = v & 0xff; }
  void sync() {
    w16(0x20, 16384 - (uint16_t)(r16(0x24) - r16(0x22)));
    w16(0x26, (uint16_t)(r16(0x2a) - r16(0x28)));
  }
  // 网线接到另一个仿真上（+nicpipe=<目录>）：发出的帧追加进 <目录>/tx，对面送来的帧从 <目录>/rx 读，
  // 每条记录是两字节长度（大端）加帧本身。两边各跑各的，只按帧对齐
  std::string pipe;
  long inoff = 0;
  void put(const std::vector<uint8_t> &f) {
    FILE *o = fopen((pipe + "/tx").c_str(), "ab");
    if (!o) return;
    uint8_t h[2] = {(uint8_t)(f.size() >> 8), (uint8_t)f.size()};
    fwrite(h, 1, 2, o), fwrite(f.data(), 1, f.size(), o), fclose(o);
  }
  void pump() {
    FILE *i = fopen((pipe + "/rx").c_str(), "rb");
    if (!i) return;
    fseek(i, 0, SEEK_END);
    long end = ftell(i);
    while (inoff + 2 <= end) {
      uint8_t h[2];
      fseek(i, inoff, SEEK_SET);
      if (fread(h, 1, 2, i) != 2) break;
      size_t n = h[0] << 8 | h[1];
      if (inoff + 2 + (long)n > end) break;
      std::vector<uint8_t> f(n);
      if (fread(f.data(), 1, n, i) != n) break;
      if (!deliver(f) && sock[0x03] == 0x42) break;
      inoff += 2 + n;
    }
    fclose(i);
  }
  // 往接收缓冲里放一帧：两字节长度（连它自己）、帧本身；放不下回 false
  bool deliver(const std::vector<uint8_t> &f) {
    if (sock[0x03] != 0x42) return false;
    uint16_t wr = r16(0x2a), used = wr - r16(0x28);
    if (used + f.size() + 2 > 16384) return false;
    uint16_t len = f.size() + 2;
    rx[wr & 0x3fff] = len >> 8, rx[(wr + 1) & 0x3fff] = len & 0xff;
    for (size_t i = 0; i < f.size(); ++i) rx[(wr + 2 + i) & 0x3fff] = f[i];
    w16(0x2a, wr + len);
    sock[0x02] |= 0x04, ++got;
    sync();
    return true;
  }
  static uint16_t csum(const uint8_t *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) s += p[i] << 8 | p[i + 1];
    if (n & 1) s += p[n - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return ~s;
  }
  // DHCP 服务（RFC 2131）：DISCOVER 回 OFFER、REQUEST 回 ACK，租出去的总是 10.0.0.2，网关与服务器是自己
  long dhcps = 0;
  void dhcp(const std::vector<uint8_t> &f, size_t at) {
    const uint8_t *q = &f[at];
    size_t n = f.size() - at;
    if (n < 244 || q[0] != 1 || q[236] != 0x63 || q[237] != 0x82 || q[238] != 0x53 || q[239] != 0x63) return;
    int type = 0;
    for (size_t i = 240; i + 1 < n && q[i] != 255;) {
      if (q[i] == 0) { ++i; continue; }
      if (q[i] == 53 && i + 2 < n) type = q[i + 2];
      i += 2 + q[i + 1];
    }
    if (type != 1 && type != 3) return;
    std::vector<uint8_t> d(300);
    d[0] = 2, d[1] = 1, d[2] = 6;
    memcpy(&d[4], q + 4, 4), memcpy(&d[10], q + 10, 2), memcpy(&d[28], q + 28, 16);
    d[16] = 10, d[17] = 0, d[18] = 0, d[19] = 2;
    memcpy(&d[20], host_ip, 4);
    d[236] = 0x63, d[237] = 0x82, d[238] = 0x53, d[239] = 0x63;
    const uint8_t opt[] = {53, 1, (uint8_t)(type == 1 ? 2 : 5), 54, 4, 10, 0, 0, 1, 1, 4, 255, 255, 255, 0,
                           3, 4, 10, 0, 0, 1, 51, 4, 0, 0, 0x0e, 0x10, 255};
    memcpy(&d[240], opt, sizeof opt);
    std::vector<uint8_t> r(14 + 20 + 8 + d.size());
    memset(&r[0], 0xff, 6), memcpy(&r[6], host_mac, 6), r[12] = 0x08, r[13] = 0x00;
    uint8_t *ip = &r[14];
    size_t tot = 20 + 8 + d.size();
    ip[0] = 0x45, ip[2] = tot >> 8, ip[3] = tot & 0xff, ip[8] = 64, ip[9] = 17;
    memcpy(ip + 12, host_ip, 4), memset(ip + 16, 0xff, 4);
    uint16_t c = csum(ip, 20);
    ip[10] = c >> 8, ip[11] = c & 0xff;
    uint8_t *u = ip + 20;
    u[1] = 67, u[3] = 68, u[4] = (8 + d.size()) >> 8, u[5] = (8 + d.size()) & 0xff;
    memcpy(u + 8, d.data(), d.size());
    ++dhcps, deliver(r);
  }
  // 那头的主机看到一帧
  void host(const std::vector<uint8_t> &f) {
    if (f.size() < 42) return;
    if (f[12] == 0x08 && f[13] == 0x00 && f[23] == 17) {
      size_t ihl = (f[14] & 15) * 4;
      if (f.size() >= 14 + ihl + 8 && f[14 + ihl + 2] == 0 && f[14 + ihl + 3] == 67) {
        dhcp(f, 14 + ihl + 8);
        return;
      }
    }
    bool to_me = !memcmp(&f[0], host_mac, 6) || (f[0] & f[1] & f[2] & f[3] & f[4] & f[5]) == 0xff;
    if (!to_me) return;
    std::vector<uint8_t> r(f);
    memcpy(&r[0], &f[6], 6), memcpy(&r[6], host_mac, 6);
    if (f[12] == 0x08 && f[13] == 0x06 && f[21] == 1 && !memcmp(&f[38], host_ip, 4)) {
      r.resize(42);
      r[21] = 2;
      memcpy(&r[32], &f[22], 10), memcpy(&r[22], host_mac, 6), memcpy(&r[28], host_ip, 4);
      ++arps, deliver(r);
    } else if (f[12] == 0x08 && f[13] == 0x00 && f[23] == 1 && !memcmp(&f[30], host_ip, 4)) {
      size_t ihl = (f[14] & 15) * 4, tot = f[16] << 8 | f[17];
      if (14 + tot > f.size() || f[14 + ihl] != 8) return;
      r.resize(14 + tot);
      memcpy(&r[26], &f[30], 4), memcpy(&r[30], &f[26], 4);
      r[24] = r[25] = 0;
      uint16_t c = csum(&r[14], ihl);
      r[24] = c >> 8, r[25] = c & 0xff;
      r[14 + ihl] = 0, r[16 + ihl] = r[17 + ihl] = 0;
      c = csum(&r[14 + ihl], tot - ihl);
      r[16 + ihl] = c >> 8, r[17 + ihl] = c & 0xff;
      ++pings, deliver(r);
    }
  }
  void command(uint8_t c) {
    switch (c) {
      case 0x01:
        sock[0x03] = (sock[0x00] & 0x0f) == 0x04 ? 0x42 : 0x13;
        w16(0x22, 0), w16(0x24, 0), w16(0x28, 0), w16(0x2a, 0);
        break;
      case 0x10: sock[0x03] = 0x00; break;
      case 0x20: {
        uint16_t rd = r16(0x22), wr = r16(0x24);
        std::vector<uint8_t> f;
        for (uint16_t i = rd; i != wr; ++i) f.push_back(tx[i & 0x3fff]);
        w16(0x22, wr);
        sock[0x02] |= 0x10, ++sent;
        if (pipe.empty()) host(f);
        else put(f);
        break;
      }
      default: break;
    }
    sock[0x01] = 0;
    sync();
  }
  uint8_t rd(int blk, uint16_t a) const {
    switch (blk) {
      case 0: return a < sizeof common ? common[a] : 0;
      case 1: return a < sizeof sock ? sock[a] : 0;
      case 2: return tx[a & 0x3fff];
      case 3: return rx[a & 0x3fff];
      default: return 0;
    }
  }
  void wr(int blk, uint16_t a, uint8_t v) {
    if (blk == 0 && a == 0 && (v & 0x80)) { reset(); return; }
    if (blk == 0 && a < sizeof common) common[a] = v;
    else if (blk == 1 && a == 0x01) command(v);
    else if (blk == 1 && a == 0x02) sock[0x02] &= ~v;
    else if (blk == 1 && a < sizeof sock && a != 0x03 && a != 0x20 && a != 0x21 && a != 0x26 && a != 0x27) {
      sock[a] = v;
      if (a == 0x25 || a == 0x29) sync();
    } else if (blk == 2) tx[a & 0x3fff] = v;
  }
  void step(bool cs, bool ck, bool mosi) {
    if (cs) { nbit = 0, n = 0, miso = true, sel = false, sclk = ck; return; }
    if (!sel) sel = true, cur = 0xff;
    if (!sclk && ck) {
      sh = sh << 1 | mosi;
      if (++nbit == 8) {
        nbit = 0;
        int blk = ctl >> 3;
        if (n == 0) addr = sh << 8;
        else if (n == 1) addr |= sh;
        else if (n == 2) ctl = sh;
        else if (ctl & 4) wr(blk, addr++, sh);
        else addr++;
        ++n;
        cur = n >= 3 && !(ctl & 4) ? rd(ctl >> 3, addr) : 0xff;
      }
    } else if (sclk && !ck) {
      miso = cur >> (7 - nbit) & 1;
    }
    sclk = ck;
  }
};

struct Uart {
  int div = 434, pace = 0;
  // 收芯片发出来的
  bool last = true;
  int rcnt = 0, rbit = -1, rsh = 0, high = 0;
  std::string seen;
  // 往芯片发
  std::deque<uint8_t> q;
  int tcnt = 0, tbit = -1, tsh = 0, gap = 0;
  bool tx = true;

  void ckpt(Ckpt &c) {
    c(last), c(rcnt), c(rbit), c(rsh), c(high), c.seq(q), c(tcnt), c(tbit), c(tsh), c(gap), c(tx);
  }
  // 复位那一段线上是低的，不是起始位：线连着高满一帧才开始认
  bool recv(bool line, char &c) {
    bool got = false;
    if (high < 10 * div) {
      high = line ? high + 1 : 0;
    } else if (rbit < 0) {
      if (last && !line) rbit = 0, rcnt = div + div / 2;
    } else if (--rcnt == 0) {
      if (rbit < 8) rsh |= line << rbit, rcnt = div, ++rbit;
      else c = rsh, got = true, rbit = -1, rsh = 0;
    }
    last = line;
    return got;
  }
  void send() {
    if (tbit < 0) {
      if (gap) { --gap; return; }
      if (q.empty()) return;
      // 起始位、八位数据、两位停止位
      tsh = (q.front() << 1) | 0x600, q.pop_front(), tbit = 0, tcnt = div;
      tx = tsh & 1;
    } else if (--tcnt == 0) {
      if (++tbit == 11) { tbit = -1, tx = true, gap = pace; return; }
      tx = tsh >> tbit & 1, tcnt = div;
    }
  }
  bool idle() const { return q.empty() && tbit < 0 && !gap; }
};

struct Step { char kind; std::string text; };

static std::string unesc(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char n = s[++i];
      o += n == 'n' ? '\n' : n == 'r' ? '\r' : n == 't' ? '\t' : n;
    } else o += s[i];
  }
  return o;
}

static bool load(std::vector<uint8_t> &buf, const std::string &spec) {
  auto p = spec.rfind('@');
  std::string file = spec.substr(0, p);
  size_t off = p == std::string::npos ? 0 : strtoull(spec.c_str() + p + 1, nullptr, 0);
  std::ifstream f(file, std::ios::binary);
  if (!f) { fprintf(stderr, "打不开 %s\n", file.c_str()); return false; }
  std::vector<char> d((std::istreambuf_iterator<char>(f)), {});
  if (off + d.size() > buf.size()) { fprintf(stderr, "%s 放不下\n", file.c_str()); return false; }
  memcpy(buf.data() + off, d.data(), d.size());
  fprintf(stderr, "%s：%zu 字节放到 0x%zx\n", file.c_str(), d.size(), off);
  return true;
}

#ifdef WITH_GPU
// 另一颗芯片的整份网表，与这颗同一个时钟。它的管理口在 payload 第 56 至 59 位，done 在第 60 位；
// 管理口没被选中时 MISO 不驱动，板上有上拉。只在 +gpu 时步进，起 Linux 那一段不受它拖慢。
struct Gpu {
  Vgpu m;
  int left = 16;
  bool miso = true, done = false, cs = true;
  long xfers = 0;
  bool get(const VlWide<3> &v, int b) { return v[b >> 5] >> (b & 31) & 1; }
  void put(int b, bool v) { m.io_in[b >> 5] = (m.io_in[b >> 5] & ~(1u << (b & 31))) | (uint32_t)v << (b & 31); }
  void step(bool csn, bool sck, bool mosi) {
    m.reset = left > 0;
    if (left) --left;
    xfers += cs && !csn;
    cs = csn;
    put(56, sck), put(57, csn), put(58, mosi);
    m.clock = 1;
    m.eval();
    miso = !get(m.io_oe, 59) || get(m.io_out, 59);
    done = get(m.io_out, 60);
    m.clock = 0;
    m.eval();
  }
};
#endif

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  Sdram sdram;
  Flash flash;
  Uart uart;
  Echo sd, net;
  SdCard card;
  W5500 nic;
  std::string sdout;
  bool spiecho = false, hasnic = false, hasgpu = false;
#ifdef WITH_GPU
  Gpu gpu;
#endif
  std::vector<Step> script;
  std::vector<std::string> rejects;
  std::string hit;
  std::string restore, stopfile;
  uint64_t max = 50'000'000, beat = 0, stop = 0;
  bool stopsec = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char *k) { return a.rfind(k, 0) == 0 ? a.c_str() + strlen(k) : nullptr; };
    if (auto v = val("+flash=")) { if (!load(flash.mem, v)) return 2; }
    else if (auto v = val("+sdram=")) {
      std::vector<uint8_t> b(32u << 20);
      if (!load(b, v)) return 2;
      for (size_t k = 0; k < b.size(); ++k) {
        uint32_t m = Sdram::wire(k);
        sdram.mem[m >> 1] |= b[k] << (m & 1 ? 8 : 0);
      }
    }
    else if (a == "+spiecho") spiecho = true;
    else if (auto v = val("+sd=")) {
      std::ifstream f(v, std::ios::binary);
      if (!f) { fprintf(stderr, "打不开 %s\n", v); return 2; }
      card.mem.assign((std::istreambuf_iterator<char>(f)), {});
      card.mem.resize((card.mem.size() + 0x7ffff) & ~(size_t)0x7ffff);
      fprintf(stderr, "SD 卡：%s，%u 块\n", v, card.blocks());
    }
    else if (auto v = val("+sdout=")) sdout = v;
    else if (a == "+w5500") hasnic = true;
    else if (a == "+gpu") {
#ifdef WITH_GPU
      hasgpu = true;
#else
      fprintf(stderr, "这个仿真器没有编进 to2610-gpu，编的时候给 SIM_GPU\n");
      return 2;
#endif
    }
    else if (auto v = val("+nicpipe=")) hasnic = true, nic.pipe = v;
    else if (auto v = val("+tohost=")) sdram.watch = Sdram::wire(strtoul(v, nullptr, 0) - 0x80000000u) >> 1;
    else if (auto v = val("+uartdiv=")) uart.div = atoi(v);
    else if (auto v = val("+pace=")) uart.pace = atoi(v);
    else if (auto v = val("+max=")) max = strtoull(v, nullptr, 0);
    else if (auto v = val("+beat=")) beat = strtoull(v, nullptr, 0);
    else if (auto v = val("+restore=")) restore = v;
    else if (auto v = val("+stop=")) {
      const char *at = strchr(v, '@');
      if (!at) { fprintf(stderr, "+stop=<周期数或秒数 s>@<断点文件>\n"); return 2; }
      char *end;
      stop = strtoull(v, &end, 0), stopsec = *end == 's', stopfile = at + 1;
    }
    else if (auto v = val("+script=")) {
      std::ifstream f(v);
      if (!f) { fprintf(stderr, "打不开 %s\n", v); return 2; }
      for (std::string l; std::getline(f, l);) {
        if (l.rfind("expect ", 0) == 0) script.push_back({'e', unesc(l.substr(7))});
        else if (l.rfind("send ", 0) == 0) script.push_back({'s', unesc(l.substr(5))});
        else if (l.rfind("save ", 0) == 0) script.push_back({'c', l.substr(5)});
        else if (l.rfind("reject ", 0) == 0) rejects.push_back(unesc(l.substr(7)));
      }
    }
  }

  auto top = new Vtb;
  uint64_t in = 1ull << pad::UART_RX | 1ull << pad::SPI0_MISO | 1ull << pad::FLASH_MISO | 1ull << pad::SPI1_MISO;
  size_t at = 0, from = 0;
  uint64_t cyc = 0;
  bool sending = false;
  // 断点存的是不是「同一份脚本接着走」：+stop 存的是，脚本里的 save 存的不是
  uint8_t cont = 0;

  // 断点分两个文件：<名字> 是 Verilator 存的片内状态，<名字>.tb 是片外模型与测试台自己的
  auto ckpt = [&](const std::string &name, bool out) {
    Ckpt c{fopen((name + ".tb").c_str(), out ? "wb" : "rb"), out};
    if (!c.f) { perror(name.c_str()); exit(2); }
    sdram.ckpt(c), flash.ckpt(c), uart.ckpt(c), sd.ckpt(c), net.ckpt(c), c(in), c(cyc);
    c(cont), c(at), c(from), c(sending), c.str(uart.seen);
    if (!out && !cont) at = from = 0, sending = false, uart.seen.clear();
    fclose(c.f);
    if (out) {
      VerilatedSave os;
      os.open(name.c_str());
      os << *top;
      os.close();
    } else {
      VerilatedRestore is;
      is.open(name.c_str());
      is >> *top;
      is.close();
    }
    fprintf(stderr, "断点 %s：第 %llu 个周期%s%s\n", name.c_str(), (unsigned long long)cyc, out ? "存下" : "，从这里接着跑",
            !out && cont ? "，脚本从停下的那一步接着走" : "");
  };

  top->rst_n = 0;
  if (!restore.empty()) ckpt(restore, false);
  if (at > script.size()) { fprintf(stderr, "断点停在脚本第 %zu 步，这份脚本只有 %zu 步\n", at, script.size()); return 2; }
  uint64_t start = cyc;
  bool stopped = false;
  auto t0 = std::chrono::steady_clock::now();
  auto waiting = [&] { return at < script.size() || (sdram.watch >= 0 && !sdram.hit); };
  for (; cyc - start < max && waiting() && hit.empty(); ++cyc) {
    if (stop && (stopsec ? (cyc & 0xfffff) == 0 &&
                               std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(stop)
                         : cyc - start >= stop)) {
      cont = 1;
      ckpt(stopfile, true);
      stopped = true;
      break;
    }
    if (at < script.size() && script[at].kind == 'c') {
      cont = 0;
      ckpt(script[at].text, true);
      ++at, from = uart.seen.size();
      continue;
    }
    if (cyc == 100) top->rst_n = 1;
    if (beat && cyc && cyc % beat == 0)
      fprintf(stderr, "[%llu 百万周期] SDRAM 读 %ld 写 %ld，串口收到 %zu 字，脚本 %zu/%zu\n",
              (unsigned long long)(cyc / 1000000), sdram.reads, sdram.writes, uart.seen.size(), at, script.size());
    top->pad_in = in;
    top->clk = 1;
    top->eval();
    uint64_t out = top->pad_out;

    flash.step(bits(out, pad::FLASH_CSN, 1), bits(out, pad::FLASH_SCLK, 1), bits(out, pad::FLASH_MOSI, 1));
    bool hascard = !card.mem.empty();
    if (hascard) card.step(bits(out, pad::SPI0_CSN, 1), bits(out, pad::SPI0_SCLK, 1), bits(out, pad::SPI0_MOSI, 1));
    if (spiecho) {
      if (!hascard) sd.step(bits(out, pad::SPI0_CSN, 1), bits(out, pad::SPI0_SCLK, 1), bits(out, pad::SPI0_MOSI, 1));
      if (!hasnic) net.step(bits(out, pad::SPI1_CSN, 1), bits(out, pad::SPI1_SCLK, 1), bits(out, pad::SPI1_MOSI, 1));
    }
    if (hasnic) {
      if (!nic.pipe.empty() && (cyc & 0xfff) == 0) nic.pump();
      nic.step(bits(out, pad::SPI1_CSN, 1), bits(out, pad::SPI1_SCLK, 1), bits(out, pad::SPI1_MOSI, 1));
    }
    bool gmiso = true, gdone = true;
#ifdef WITH_GPU
    if (hasgpu) {
      gpu.step(bits(out, pad::SPI1_CSN, 1), bits(out, pad::SPI1_SCLK, 1), bits(out, pad::SPI1_MOSI, 1));
      gmiso = gpu.miso, gdone = gpu.done;
    }
#endif
    // GPIO 脚上有上拉，接了 gpu 时是它的 done；芯片驱动时读回的是它自己驱动的电平
    bool gpio = bits(top->pad_oe, pad::GPIO, 1) ? bits(out, pad::GPIO, 1) : gdone;
    char c;
    if (uart.recv(bits(out, pad::UART_TX, 1), c)) {
      putchar(c);
      fflush(stdout);
      uart.seen += c;
      for (auto &r : rejects)
        if (uart.seen.size() >= r.size() && !uart.seen.compare(uart.seen.size() - r.size(), r.size(), r)) hit = r;
    }
    uart.send();

    if (at >= script.size()) {
    } else if (script[at].kind == 's') {
      if (!sending) {
        for (char ch : script[at].text) uart.q.push_back(ch);
        sending = true, from = uart.seen.size();
      } else if (uart.idle()) sending = false, ++at;
    } else if (uart.seen.find(script[at].text, from) != std::string::npos) {
      ++at, from = uart.seen.size();
    }

    in = (in & ~(1ull << pad::UART_RX | 1ull << pad::FLASH_MISO | 1ull << pad::SPI0_MISO | 1ull << pad::SPI1_MISO |
                 1ull << pad::GPIO)) |
         (uint64_t)uart.tx << pad::UART_RX | (uint64_t)flash.miso << pad::FLASH_MISO |
         (uint64_t)(hascard ? card.miso : !spiecho || sd.miso) << pad::SPI0_MISO |
         (uint64_t)(hasnic ? nic.miso : hasgpu ? gmiso : !spiecho || net.miso) << pad::SPI1_MISO |
         (uint64_t)gpio << pad::GPIO;
    top->pad_in = in;
    top->clk = 0;
    top->eval();
    // sdram_clk 是 clk 取反：这里正是它的上升沿
    sdram.clk(top->pad_out);
    in = (in & ~(0xffffull << pad::SD_DQ)) | (uint64_t)sdram.dq << pad::SD_DQ;
  }
  fflush(stdout);
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  bool ok = stopped || (hit.empty() && (sdram.watch >= 0 ? sdram.hit && sdram.host == 1 : at == script.size() && !script.empty()));
  if (stopped)
    fprintf(stderr, "\n停在这里：脚本走到 %zu/%zu，断点 %s；同一份脚本加 +restore=%s 接着跑", at, script.size(), stopfile.c_str(),
            stopfile.c_str());
  if (sdram.watch >= 0 && !sdram.hit && !stopped) fprintf(stderr, "\n没等到程序写 tohost");
  if (sdram.hit && sdram.host != 1) fprintf(stderr, "\ntohost 写的是 %u：第 %u 项不过", sdram.host, sdram.host >> 1);
  fprintf(stderr, "\n%s：到第 %llu 个周期，这次跑了 %llu 个，%.0f 秒，每秒 %.0f 千周期；SDRAM 读 %ld 写 %ld；脚本走到 %zu/%zu\n",
          ok ? "过" : "没过", (unsigned long long)cyc, (unsigned long long)(cyc - start), s, (cyc - start) / s / 1e3,
          sdram.reads, sdram.writes, at, script.size());
  if (!card.mem.empty()) fprintf(stderr, "SD 卡读 %ld 块、写 %ld 块\n", card.reads, card.writes);
  if (hasnic) fprintf(stderr, "网卡发 %ld 帧、收 %ld 帧；那头的主机应 ARP %ld 次、回 ping %ld 次、答 DHCP %ld 次\n", nic.sent, nic.got,
                      nic.arps, nic.pings, nic.dhcps);
#ifdef WITH_GPU
  if (hasgpu) fprintf(stderr, "gpu 的管理口收到 %ld 次传输，done 停在 %d\n", gpu.xfers, gpu.done);
#endif
  if (!sdout.empty()) std::ofstream(sdout, std::ios::binary).write((const char *)card.mem.data(), card.mem.size());
  if (!hit.empty()) fprintf(stderr, "撞上：reject %s\n", hit.c_str());
  else if (!ok && at < script.size()) fprintf(stderr, "卡在：%s %s\n", script[at].kind == 's' ? "send" : "expect", script[at].text.c_str());
  top->final();
  delete top;
  return ok ? 0 : 1;
}
