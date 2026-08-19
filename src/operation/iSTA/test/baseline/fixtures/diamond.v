module stage0_diamond(clk, data_in, data_out);
  input clk;
  input data_in;
  output data_out;

  wire stem;
  wire branch_a;
  wire branch_b;

  BUF_X1 u_stem (.A(data_in), .Y(stem));
  BUF_X1 u_a (.A(stem), .Y(branch_a));
  INV_X1 u_b (.A(stem), .Y(branch_b));
  OR2_X1 u_join (.A(branch_a), .B(branch_b), .Y(data_out));
endmodule
