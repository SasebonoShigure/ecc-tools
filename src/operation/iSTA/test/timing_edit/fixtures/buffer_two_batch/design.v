module stage4_buffer_two_batch(clk, data_in, data_out);
  input clk;
  input data_in;
  output data_out;

  // Both loads begin on one net. Round one moves exactly one load; round two
  // can then select the created output net as a distinct semantic net.
  OR2_X1 u0 (.A(data_in), .B(data_in), .Y(data_out));
endmodule
