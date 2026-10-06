`timescale 1ns/1ps
`default_nettype none

module hygcn_combination_cluster #(
    parameter int MODULES = 8,
    parameter int ARRAYS_PER_MODULE = 4,
    parameter int LANES = 128,
    parameter int DATA_WIDTH = 16,
    parameter int WEIGHT_WIDTH = 16,
    parameter int ACC_WIDTH = 48,
    parameter int BATCH_ID_WIDTH = 16,
    parameter int VERTEX_ID_WIDTH = 32,
    parameter int OUTPUTS = MODULES * ARRAYS_PER_MODULE
) (
    input  logic                                      clk,
    input  logic                                      rst_n,

    input  logic                                      s_valid,
    output logic                                      s_ready,
    input  logic [BATCH_ID_WIDTH-1:0]                 s_batch_id,
    input  logic [VERTEX_ID_WIDTH-1:0]                s_vertex_id,
    input  logic                                      s_first,
    input  logic                                      s_last,
    input  logic [LANES*DATA_WIDTH-1:0]               s_activation,
    input  logic [OUTPUTS*LANES*WEIGHT_WIDTH-1:0]     s_weight,
    input  logic [OUTPUTS*ACC_WIDTH-1:0]              s_bias,

    output logic                                      m_valid,
    input  logic                                      m_ready,
    output logic [BATCH_ID_WIDTH-1:0]                 m_batch_id,
    output logic [VERTEX_ID_WIDTH-1:0]                m_vertex_id,
    output logic [OUTPUTS*ACC_WIDTH-1:0]              m_data,

    output logic                                      active,
    output logic                                      protocol_error
);
    localparam int PRODUCT_WIDTH = DATA_WIDTH + WEIGHT_WIDTH;

    logic [BATCH_ID_WIDTH-1:0] active_batch_id;
    logic [VERTEX_ID_WIDTH-1:0] active_vertex_id;
    logic signed [ACC_WIDTH-1:0] accumulator [0:OUTPUTS-1];
    logic signed [ACC_WIDTH-1:0] next_accumulator [0:OUTPUTS-1];
    logic signed [PRODUCT_WIDTH-1:0] product [0:OUTPUTS-1][0:LANES-1];

    genvar product_output;
    genvar product_lane;
    generate
        for (product_output = 0;
             product_output < OUTPUTS;
             product_output = product_output + 1) begin : output_products
            for (product_lane = 0;
                 product_lane < LANES;
                 product_lane = product_lane + 1) begin : lane_products
                assign product[product_output][product_lane] =
                    $signed(s_activation[product_lane*DATA_WIDTH +: DATA_WIDTH]) *
                    $signed(s_weight[(product_output*LANES+product_lane)*WEIGHT_WIDTH +:
                                     WEIGHT_WIDTH]);
            end
        end
    endgenerate

    integer combinational_output;
    integer combinational_lane;
    always_comb begin
        for (combinational_output = 0;
             combinational_output < OUTPUTS;
             combinational_output = combinational_output + 1) begin
            if (s_first) begin
                next_accumulator[combinational_output] =
                    $signed(s_bias[combinational_output*ACC_WIDTH +: ACC_WIDTH]);
            end else begin
                next_accumulator[combinational_output] = accumulator[combinational_output];
            end

            for (combinational_lane = 0;
                combinational_lane < LANES;
                 combinational_lane = combinational_lane + 1) begin
                next_accumulator[combinational_output] =
                    next_accumulator[combinational_output] +
                    $signed({{(ACC_WIDTH-PRODUCT_WIDTH){
                                   product[combinational_output][combinational_lane]
                                       [PRODUCT_WIDTH-1]}},
                              product[combinational_output][combinational_lane]});
            end
        end
    end

    assign s_ready = !m_valid || m_ready;

    integer sequential_output;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            m_valid <= 1'b0;
            m_batch_id <= '0;
            m_vertex_id <= '0;
            m_data <= '0;
            active <= 1'b0;
            active_batch_id <= '0;
            active_vertex_id <= '0;
            protocol_error <= 1'b0;
            for (sequential_output = 0;
                 sequential_output < OUTPUTS;
                 sequential_output = sequential_output + 1) begin
                accumulator[sequential_output] <= '0;
            end
        end else begin
            if (m_valid && m_ready) begin
                m_valid <= 1'b0;
            end

            if (s_valid && s_ready) begin
                if ((!active && !s_first) ||
                    (active && s_first) ||
                    (active && ((s_batch_id != active_batch_id) ||
                                (s_vertex_id != active_vertex_id)))) begin
                    protocol_error <= 1'b1;
                end

                if (s_first) begin
                    active_batch_id <= s_batch_id;
                    active_vertex_id <= s_vertex_id;
                end

                for (sequential_output = 0;
                     sequential_output < OUTPUTS;
                     sequential_output = sequential_output + 1) begin
                    accumulator[sequential_output] <= next_accumulator[sequential_output];
                    if (s_last) begin
                        m_data[sequential_output*ACC_WIDTH +: ACC_WIDTH] <=
                            next_accumulator[sequential_output];
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
