module stage4_buffer_terminal_empty(
    clk, dummy_in0, dummy_in1, dummy_in2, target_in,
    dummy_out0, dummy_out1, dummy_out2, guard_out, data_out);
  input clk;
  input dummy_in0;
  input dummy_in1;
  input dummy_in2;
  input target_in;
  output dummy_out0;
  output dummy_out1;
  output dummy_out2;
  output guard_out;
  output data_out;

  // These three input nets have Cload == Cbuf and therefore negative gain.
  BUF_X1 d0 (.A(dummy_in0), .Y(dummy_out0));
  BUF_X1 d1 (.A(dummy_in1), .Y(dummy_out1));
  BUF_X1 d2 (.A(dummy_in2), .Y(dummy_out2));
  // The guard makes target_in critical without changing target:A's positive gain.
  BUF_X1 guard (.A(target_in), .Y(guard_out));
  // The virtual clock stays outside the timing DAG.
  OR2_X1 target (.A(target_in), .B(), .Y(data_out));
endmodule
