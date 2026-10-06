// 整颗 KianV SoC 的测试台。片外的 SDRAM、SPI Flash 与串口按引脚电平建模，接在一条 54 位的焊盘总线上，
// 位次照上游 chip_core.sv；上游源码与流片交付的那份展平文件各包一层 tb，用的是同一份测试台。
//
//   +flash=<文件>@<偏移>   往 Flash 里放一段，可多次给
//   +sdram=<文件>@<偏移>   直接写进 SDRAM，只给调试用
//   +script=<文件>         逐行 expect <文本> / send <文本> / save <文件>，全部走完算过
//   +restore=<文件>        从 save 存下的断点接着跑，脚本从头走
//   +uartdiv=<n>           串口每位占几个时钟
//   +pace=<n>              往芯片发的相邻两个字之间空几个时钟
//   +max=<n>               最多跑几个时钟周期，到了还没走完算不过
//   +beat=<n>              每 n 个周期往标准错误报一次进度：小时级的仿真要看得出它还活着
//
// 断点是给 Linux 用的：起到 shell 要仿一个多小时，存一次，之后调命令从断点起。
// 片上的串口接收缓冲只有 16 个字，内核又是按时钟节拍去取的，一口气发一整行会冲掉，所以要 +pace。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

#include "Vtb.h"
#include "verilated.h"
#include "verilated_save.h"

namespace pad {
enum : int {
  UART_RX = 0, SPI0_MISO = 1, FLASH_MISO = 2, SPI1_MISO = 3,
  UART_TX = 4, FLASH_CSN = 8, FLASH_SCLK = 9, FLASH_MOSI = 10,
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

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  Sdram sdram;
  Flash flash;
  Uart uart;
  std::vector<Step> script;
  std::string restore;
  uint64_t max = 50'000'000, beat = 0;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char *k) { return a.rfind(k, 0) == 0 ? a.c_str() + strlen(k) : nullptr; };
    if (auto v = val("+flash=")) { if (!load(flash.mem, v)) return 2; }
    else if (auto v = val("+sdram=")) {
      std::vector<uint8_t> b(32u << 20);
      if (!load(b, v)) return 2;
      for (size_t k = 0; k < b.size() / 2; ++k) sdram.mem[k] |= b[2 * k] | b[2 * k + 1] << 8;
    }
    else if (auto v = val("+uartdiv=")) uart.div = atoi(v);
    else if (auto v = val("+pace=")) uart.pace = atoi(v);
    else if (auto v = val("+max=")) max = strtoull(v, nullptr, 0);
    else if (auto v = val("+beat=")) beat = strtoull(v, nullptr, 0);
    else if (auto v = val("+restore=")) restore = v;
    else if (auto v = val("+script=")) {
      std::ifstream f(v);
      if (!f) { fprintf(stderr, "打不开 %s\n", v); return 2; }
      for (std::string l; std::getline(f, l);) {
        if (l.rfind("expect ", 0) == 0) script.push_back({'e', unesc(l.substr(7))});
        else if (l.rfind("send ", 0) == 0) script.push_back({'s', unesc(l.substr(5))});
        else if (l.rfind("save ", 0) == 0) script.push_back({'c', l.substr(5)});
      }
    }
  }

  auto top = new Vtb;
  uint64_t in = 1ull << pad::UART_RX | 1ull << pad::SPI0_MISO | 1ull << pad::FLASH_MISO | 1ull << pad::SPI1_MISO;
  size_t at = 0, from = 0;
  uint64_t cyc = 0;
  bool sending = false;

  // 断点分两个文件：<名字> 是 Verilator 存的片内状态，<名字>.tb 是片外模型与测试台自己的
  auto ckpt = [&](const std::string &name, bool out) {
    Ckpt c{fopen((name + ".tb").c_str(), out ? "wb" : "rb"), out};
    if (!c.f) { perror(name.c_str()); exit(2); }
    sdram.ckpt(c), flash.ckpt(c), uart.ckpt(c), c(in), c(cyc);
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
    fprintf(stderr, "断点 %s：第 %llu 个周期%s\n", name.c_str(), (unsigned long long)cyc, out ? "存下" : "，从这里接着跑");
  };

  top->rst_n = 0;
  if (!restore.empty()) ckpt(restore, false);
  uint64_t start = cyc;
  auto t0 = std::chrono::steady_clock::now();
  for (; cyc - start < max && at < script.size(); ++cyc) {
    if (script[at].kind == 'c') {
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
    char c;
    if (uart.recv(bits(out, pad::UART_TX, 1), c)) {
      putchar(c);
      fflush(stdout);
      uart.seen += c;
    }
    uart.send();

    if (script[at].kind == 's') {
      if (!sending) {
        for (char ch : script[at].text) uart.q.push_back(ch);
        sending = true, from = uart.seen.size();
      } else if (uart.idle()) sending = false, ++at;
    } else if (uart.seen.find(script[at].text, from) != std::string::npos) {
      ++at, from = uart.seen.size();
    }

    in = (in & ~(1ull << pad::UART_RX | 1ull << pad::FLASH_MISO)) |
         (uint64_t)uart.tx << pad::UART_RX | (uint64_t)flash.miso << pad::FLASH_MISO;
    top->pad_in = in;
    top->clk = 0;
    top->eval();
    // sdram_clk 是 clk 取反：这里正是它的上升沿
    sdram.clk(top->pad_out);
    in = (in & ~(0xffffull << pad::SD_DQ)) | (uint64_t)sdram.dq << pad::SD_DQ;
  }
  fflush(stdout);
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  bool ok = at == script.size() && !script.empty();
  fprintf(stderr, "\n%s：到第 %llu 个周期，这次跑了 %llu 个，%.0f 秒，每秒 %.0f 千周期；SDRAM 读 %ld 写 %ld；脚本走到 %zu/%zu\n",
          ok ? "过" : "没过", (unsigned long long)cyc, (unsigned long long)(cyc - start), s, (cyc - start) / s / 1e3,
          sdram.reads, sdram.writes, at, script.size());
  if (!ok && at < script.size()) fprintf(stderr, "卡在：%s %s\n", script[at].kind == 's' ? "send" : "expect", script[at].text.c_str());
  top->final();
  delete top;
  return ok ? 0 : 1;
}
