`timescale 1ns/1ps
`default_nettype none

module hygcn_aggregation_cluster #(
    parameter int CORES = 32,
    parameter int DATA_WIDTH = 16,
    parameter int ACC_WIDTH = 32,
    parameter int LANES = 16,
    parameter int BATCH_ID_WIDTH = 16,
    parameter int VERTEX_ID_WIDTH = 32
) (
    input  logic                                      clk,
    input  logic                                      rst_n,

    input  logic [CORES-1:0]                         s_valid,
    output logic [CORES-1:0]                         s_ready,
    input  logic [CORES*BATCH_ID_WIDTH-1:0]          s_batch_id,
    input  logic [CORES*VERTEX_ID_WIDTH-1:0]         s_vertex_id,
    input  logic [CORES-1:0]                         s_first,
    input  logic [CORES-1:0]                         s_last,
    input  logic [CORES-1:0]                         s_op_max,
    input  logic [CORES*LANES*DATA_WIDTH-1:0]        s_data,

    output logic [CORES-1:0]                         m_valid,
    input  logic [CORES-1:0]                         m_ready,
    output logic [CORES*BATCH_ID_WIDTH-1:0]          m_batch_id,
    output logic [CORES*VERTEX_ID_WIDTH-1:0]         m_vertex_id,
    output logic [CORES*LANES*ACC_WIDTH-1:0]         m_data,
    output logic [CORES-1:0]                         active,
    output logic [CORES-1:0]                         protocol_error
);
    genvar core;
    generate
        for (core = 0; core < CORES; core = core + 1) begin : aggregation_cores
            hygcn_aggregation_engine #(
                .DATA_WIDTH(DATA_WIDTH),
                .ACC_WIDTH(ACC_WIDTH),
                .LANES(LANES),
                .BATCH_ID_WIDTH(BATCH_ID_WIDTH),
                .VERTEX_ID_WIDTH(VERTEX_ID_WIDTH)
            ) engine (
                .clk,
                .rst_n,
                .s_valid(s_valid[core]),
                .s_ready(s_ready[core]),
                .s_batch_id(s_batch_id[core*BATCH_ID_WIDTH +: BATCH_ID_WIDTH]),
                .s_vertex_id(s_vertex_id[core*VERTEX_ID_WIDTH +: VERTEX_ID_WIDTH]),
                .s_first(s_first[core]),
                .s_last(s_last[core]),
                .s_op_max(s_op_max[core]),
                .s_data(s_data[core*LANES*DATA_WIDTH +: LANES*DATA_WIDTH]),
                .m_valid(m_valid[core]),
                .m_ready(m_ready[core]),
                .m_batch_id(m_batch_id[core*BATCH_ID_WIDTH +: BATCH_ID_WIDTH]),
                .m_vertex_id(m_vertex_id[core*VERTEX_ID_WIDTH +: VERTEX_ID_WIDTH]),
                .m_data(m_data[core*LANES*ACC_WIDTH +: LANES*ACC_WIDTH]),
                .active(active[core]),
                .protocol_error(protocol_error[core])
            );
        end
    endgenerate
endmodule

`default_nettype wire
