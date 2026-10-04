// 上游的 chip_core 接到测试台的焊盘总线上；焊盘配置线没有去处
module tb (
  input  wire        clk,
  input  wire        rst_n,
  input  wire [53:0] pad_in,
  output wire [53:0] pad_out,
  output wire [53:0] pad_oe
);
  chip_core dut (
    .clk(clk), .rst_n(rst_n),
    .bidir_in(pad_in), .bidir_out(pad_out), .bidir_oe(pad_oe),
    .bidir_cs(), .bidir_sl(), .bidir_ie(), .bidir_pu(), .bidir_pd()
  );
endmodule
