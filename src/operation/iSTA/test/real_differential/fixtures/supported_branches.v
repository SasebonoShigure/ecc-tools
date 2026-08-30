module supported_branches(
  clk_a,
  clk_b,
  data_a,
  data_b,
  disabled_in,
  near_out,
  secondary_out
);
  input clk_a;
  input clk_b;
  input data_a;
  input data_b;
  input disabled_in;
  output near_out;
  output secondary_out;

  wire stem;
  wire branch_a;
  wire branch_b;
  wire joined;
  wire loop_a;
  wire loop_b;
  wire disabled_y;
  wire disabled_loop_a;
  wire disabled_loop_b;

  PARALLEL_X1 u_parallel (.A(data_a), .Y(stem));
  NEAR_A_X1 u_near_a (.A(stem), .Y(branch_a));
  NEAR_B_X1 u_near_b (.A(stem), .Y(branch_b));
  JOIN2_X1 u_join (.A(branch_a), .B(branch_b), .Y(joined));
  NONUNATE2_X1 u_nonunate (.A(joined), .B(data_a), .Y(near_out));
  SECONDARY_X1 u_secondary (.A(data_b), .Y(secondary_out));
  LOOP_X1 u_disabled_loop_a (.A(disabled_loop_b), .Y(disabled_loop_a));
  LOOP_X1 u_disabled_loop_b (.A(disabled_loop_a), .Y(disabled_loop_b));
  DISABLE_X1 u_disabled (.A(disabled_loop_a), .Y(disabled_y));
  LOOP_X1 u_loop_a (.A(loop_b), .Y(loop_a));
  LOOP_X1 u_loop_b (.A(loop_a), .Y(loop_b));
endmodule
