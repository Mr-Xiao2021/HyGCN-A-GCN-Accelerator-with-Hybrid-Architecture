`timescale 1ns/1ps
`default_nettype none

module hygcn_aggregation_engine #(
    parameter int DATA_WIDTH = 16,
    parameter int ACC_WIDTH = 32,
    parameter int LANES = 16,
    parameter int BATCH_ID_WIDTH = 16,
    parameter int VERTEX_ID_WIDTH = 32
) (
    input  logic                              clk,
    input  logic                              rst_n,

    input  logic                              s_valid,
    output logic                              s_ready,
    input  logic [BATCH_ID_WIDTH-1:0]         s_batch_id,
    input  logic [VERTEX_ID_WIDTH-1:0]        s_vertex_id,
    input  logic                              s_first,
    input  logic                              s_last,
    input  logic                              s_op_max,
    input  logic [LANES*DATA_WIDTH-1:0]       s_data,

    output logic                              m_valid,
    input  logic                              m_ready,
    output logic [BATCH_ID_WIDTH-1:0]         m_batch_id,
    output logic [VERTEX_ID_WIDTH-1:0]        m_vertex_id,
    output logic [LANES*ACC_WIDTH-1:0]        m_data,

    output logic                              active,
    output logic                              protocol_error
);
    logic [BATCH_ID_WIDTH-1:0] active_batch_id;
    logic [VERTEX_ID_WIDTH-1:0] active_vertex_id;
    logic active_op_max;
    logic signed [ACC_WIDTH-1:0] accumulator [0:LANES-1];
    logic signed [ACC_WIDTH-1:0] next_accumulator [0:LANES-1];

    integer combinational_lane;
    always_comb begin
        for (combinational_lane = 0;
             combinational_lane < LANES;
             combinational_lane = combinational_lane + 1) begin
            if (s_first) begin
                next_accumulator[combinational_lane] =
                    {{(ACC_WIDTH-DATA_WIDTH){s_data[combinational_lane*DATA_WIDTH+DATA_WIDTH-1]}},
                     s_data[combinational_lane*DATA_WIDTH +: DATA_WIDTH]};
            end else if (active_op_max) begin
                if ($signed({{(ACC_WIDTH-DATA_WIDTH){
                                  s_data[combinational_lane*DATA_WIDTH+DATA_WIDTH-1]}},
                             s_data[combinational_lane*DATA_WIDTH +: DATA_WIDTH]}) >
                    $signed(accumulator[combinational_lane])) begin
                    next_accumulator[combinational_lane] =
                        {{(ACC_WIDTH-DATA_WIDTH){
                              s_data[combinational_lane*DATA_WIDTH+DATA_WIDTH-1]}},
                         s_data[combinational_lane*DATA_WIDTH +: DATA_WIDTH]};
                end else begin
                    next_accumulator[combinational_lane] = accumulator[combinational_lane];
                end
            end else begin
                next_accumulator[combinational_lane] = accumulator[combinational_lane] +
                    $signed({{(ACC_WIDTH-DATA_WIDTH){
                                  s_data[combinational_lane*DATA_WIDTH+DATA_WIDTH-1]}},
                             s_data[combinational_lane*DATA_WIDTH +: DATA_WIDTH]});
            end
        end
    end

    assign s_ready = !m_valid || m_ready;

    integer sequential_lane;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            m_valid <= 1'b0;
            m_batch_id <= '0;
            m_vertex_id <= '0;
            m_data <= '0;
            active <= 1'b0;
            active_batch_id <= '0;
            active_vertex_id <= '0;
            active_op_max <= 1'b0;
            protocol_error <= 1'b0;
            for (sequential_lane = 0;
                 sequential_lane < LANES;
                 sequential_lane = sequential_lane + 1) begin
                accumulator[sequential_lane] <= '0;
            end
        end else begin
            if (m_valid && m_ready) begin
                m_valid <= 1'b0;
            end

            if (s_valid && s_ready) begin
                if ((!active && !s_first) ||
                    (active && s_first) ||
                    (active && ((s_batch_id != active_batch_id) ||
                                (s_vertex_id != active_vertex_id) ||
                                (s_op_max != active_op_max)))) begin
                    protocol_error <= 1'b1;
                end

                if (s_first) begin
                    active_batch_id <= s_batch_id;
                    active_vertex_id <= s_vertex_id;
                    active_op_max <= s_op_max;
                end

                for (sequential_lane = 0;
                     sequential_lane < LANES;
                     sequential_lane = sequential_lane + 1) begin
                    accumulator[sequential_lane] <= next_accumulator[sequential_lane];
                    if (s_last) begin
                        m_data[sequential_lane*ACC_WIDTH +: ACC_WIDTH] <=
                            next_accumulator[sequential_lane];
                    end
                end

                if (s_last) begin
                    m_valid <= 1'b1;
                    m_batch_id <= s_batch_id;
                    m_vertex_id <= s_vertex_id;
                    active <= 1'b0;
                end else begin
                    active <= 1'b1;
                end
            end
        end
    end
endmodule

`default_nettype wire
