module stage10_sequential(clk, din, other, reset_n, out, qtap);
  input clk, din, other, reset_n;
  output out, qtap;
  wire ck0, ck1, d0, q0, merged, d1;
  BUF_X1 clock_root (.A(clk), .Y(ck0));
  BUF_X1 clock_branch (.A(ck0), .Y(ck1));
  BUF_X1 input_buffer (.A(din), .Y(d0));
  DFF_R launch (.CK(ck0), .D(d0), .RN(reset_n), .Q(q0));
  OR2_X1 competing_paths (.A(q0), .B(other), .Y(merged));
  BUF_X1 data_buffer (.A(merged), .Y(d1));
  DFF_F capture (.CK(ck1), .D(d1), .RN(reset_n), .Q(out));
  BUF_X1 tap_buffer (.A(q0), .Y(qtap));
endmodule
