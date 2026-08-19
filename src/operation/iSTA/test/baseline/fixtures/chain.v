module stage0_chain(clk, data_in, data_out);
  input clk;
  input data_in;
  output data_out;

  wire n0;
  wire n1;

  BUF_X1 u0 (.A(data_in), .Y(n0));
  INV_X1 u1 (.A(n0), .Y(n1));
  BUF_X1 u2 (.A(n1), .Y(data_out));
endmodule
