module stage4_buffer_fanout(
    clk, data_in, aux0, aux1, aux2, aux3, data_out);
  input clk;
  input data_in;
  input aux0;
  input aux1;
  input aux2;
  input aux3;
  output data_out;

  wire y0;
  wire y1;
  wire y2;
  wire y3;
  wire y4;
  wire y5;

  OR2_X1 u0 (.A(data_in), .B(aux0), .Y(y0));
  OR2_X1 u1 (.A(data_in), .B(aux1), .Y(y1));
  OR2_X1 u2 (.A(data_in), .B(aux2), .Y(y2));
  OR2_X1 u3 (.A(data_in), .B(aux3), .Y(y3));
  OR2_X1 u4 (.A(y0), .B(y1), .Y(y4));
  OR2_X1 u5 (.A(y2), .B(y3), .Y(y5));
  OR2_X1 u6 (.A(y4), .B(y5), .Y(data_out));
endmodule
