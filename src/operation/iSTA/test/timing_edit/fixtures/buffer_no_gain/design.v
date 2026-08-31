module stage4_buffer_no_gain(
    clk, data_in, out0, out1, out2, out3);
  input clk;
  input data_in;
  output out0;
  output out1;
  output out2;
  output out3;

  BUF_X1 b0 (.A(data_in), .Y(out0));
  BUF_X1 b1 (.A(data_in), .Y(out1));
  BUF_X1 b2 (.A(data_in), .Y(out2));
  BUF_X1 b3 (.A(data_in), .Y(out3));
endmodule
