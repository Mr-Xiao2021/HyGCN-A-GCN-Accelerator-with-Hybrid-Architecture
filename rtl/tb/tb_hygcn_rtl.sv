`timescale 1ns/1ps
`default_nettype none

module tb_hygcn_rtl;
    localparam int DATA_WIDTH = 16;
    localparam int ACC_WIDTH = 32;
    localparam int LANES = 4;
    localparam int BATCH_ID_WIDTH = 8;
    localparam int VERTEX_ID_WIDTH = 16;
    localparam int MODULES = 2;
    localparam int ARRAYS_PER_MODULE = 2;
    localparam int OUTPUTS = MODULES * ARRAYS_PER_MODULE;

    logic clk = 1'b0;
    logic rst_n = 1'b0;
    always #5 clk = ~clk;

    logic agg_s_valid;
    logic agg_s_ready;
    logic [BATCH_ID_WIDTH-1:0] agg_s_batch_id;
    logic [VERTEX_ID_WIDTH-1:0] agg_s_vertex_id;
    logic agg_s_first;
    logic agg_s_last;
    logic agg_s_op_max;
    logic [LANES*DATA_WIDTH-1:0] agg_s_data;
    logic agg_m_valid;
    logic agg_m_ready;
    logic [BATCH_ID_WIDTH-1:0] agg_m_batch_id;
    logic [VERTEX_ID_WIDTH-1:0] agg_m_vertex_id;
    logic [LANES*ACC_WIDTH-1:0] agg_m_data;
    logic agg_active;
    logic agg_protocol_error;

    hygcn_aggregation_engine #(
        .DATA_WIDTH(DATA_WIDTH),
        .ACC_WIDTH(ACC_WIDTH),
        .LANES(LANES),
        .BATCH_ID_WIDTH(BATCH_ID_WIDTH),
        .VERTEX_ID_WIDTH(VERTEX_ID_WIDTH)
    ) aggregation_engine (
        .clk,
        .rst_n,
        .s_valid(agg_s_valid),
        .s_ready(agg_s_ready),
        .s_batch_id(agg_s_batch_id),
        .s_vertex_id(agg_s_vertex_id),
        .s_first(agg_s_first),
        .s_last(agg_s_last),
        .s_op_max(agg_s_op_max),
        .s_data(agg_s_data),
        .m_valid(agg_m_valid),
        .m_ready(agg_m_ready),
        .m_batch_id(agg_m_batch_id),
        .m_vertex_id(agg_m_vertex_id),
        .m_data(agg_m_data),
        .active(agg_active),
        .protocol_error(agg_protocol_error)
    );

    localparam int AGG_CORES = 2;
    logic [AGG_CORES-1:0] cluster_s_valid;
    logic [AGG_CORES-1:0] cluster_s_ready;
    logic [AGG_CORES*BATCH_ID_WIDTH-1:0] cluster_s_batch_id;
    logic [AGG_CORES*VERTEX_ID_WIDTH-1:0] cluster_s_vertex_id;
    logic [AGG_CORES-1:0] cluster_s_first;
    logic [AGG_CORES-1:0] cluster_s_last;
    logic [AGG_CORES-1:0] cluster_s_op_max;
    logic [AGG_CORES*LANES*DATA_WIDTH-1:0] cluster_s_data;
    logic [AGG_CORES-1:0] cluster_m_valid;
    logic [AGG_CORES-1:0] cluster_m_ready;
    logic [AGG_CORES*BATCH_ID_WIDTH-1:0] cluster_m_batch_id;
    logic [AGG_CORES*VERTEX_ID_WIDTH-1:0] cluster_m_vertex_id;
    logic [AGG_CORES*LANES*ACC_WIDTH-1:0] cluster_m_data;
    logic [AGG_CORES-1:0] cluster_active;
    logic [AGG_CORES-1:0] cluster_protocol_error;

    hygcn_aggregation_cluster #(
        .CORES(AGG_CORES),
        .DATA_WIDTH(DATA_WIDTH),
        .ACC_WIDTH(ACC_WIDTH),
        .LANES(LANES),
        .BATCH_ID_WIDTH(BATCH_ID_WIDTH),
        .VERTEX_ID_WIDTH(VERTEX_ID_WIDTH)
    ) aggregation_cluster (
        .clk,
        .rst_n,
        .s_valid(cluster_s_valid),
        .s_ready(cluster_s_ready),
        .s_batch_id(cluster_s_batch_id),
        .s_vertex_id(cluster_s_vertex_id),
        .s_first(cluster_s_first),
        .s_last(cluster_s_last),
        .s_op_max(cluster_s_op_max),
        .s_data(cluster_s_data),
        .m_valid(cluster_m_valid),
        .m_ready(cluster_m_ready),
        .m_batch_id(cluster_m_batch_id),
        .m_vertex_id(cluster_m_vertex_id),
        .m_data(cluster_m_data),
        .active(cluster_active),
        .protocol_error(cluster_protocol_error)
    );

    logic ce_s_valid;
    logic ce_s_ready;
    logic [BATCH_ID_WIDTH-1:0] ce_s_batch_id;
    logic [VERTEX_ID_WIDTH-1:0] ce_s_vertex_id;
    logic ce_s_first;
    logic ce_s_last;
    logic [LANES*DATA_WIDTH-1:0] ce_s_activation;
    logic [OUTPUTS*LANES*DATA_WIDTH-1:0] ce_s_weight;
    logic [OUTPUTS*ACC_WIDTH-1:0] ce_s_bias;
    logic ce_m_valid;
    logic ce_m_ready;
    logic [BATCH_ID_WIDTH-1:0] ce_m_batch_id;
    logic [VERTEX_ID_WIDTH-1:0] ce_m_vertex_id;
    logic [OUTPUTS*ACC_WIDTH-1:0] ce_m_data;
    logic ce_active;
    logic ce_protocol_error;

    hygcn_combination_cluster #(
        .MODULES(MODULES),
        .ARRAYS_PER_MODULE(ARRAYS_PER_MODULE),
        .LANES(LANES),
        .DATA_WIDTH(DATA_WIDTH),
        .WEIGHT_WIDTH(DATA_WIDTH),
        .ACC_WIDTH(ACC_WIDTH),
        .BATCH_ID_WIDTH(BATCH_ID_WIDTH),
        .VERTEX_ID_WIDTH(VERTEX_ID_WIDTH)
    ) combination_cluster (
        .clk,
        .rst_n,
        .s_valid(ce_s_valid),
        .s_ready(ce_s_ready),
        .s_batch_id(ce_s_batch_id),
        .s_vertex_id(ce_s_vertex_id),
        .s_first(ce_s_first),
        .s_last(ce_s_last),
        .s_activation(ce_s_activation),
        .s_weight(ce_s_weight),
        .s_bias(ce_s_bias),
        .m_valid(ce_m_valid),
        .m_ready(ce_m_ready),
        .m_batch_id(ce_m_batch_id),
        .m_vertex_id(ce_m_vertex_id),
        .m_data(ce_m_data),
        .active(ce_active),
        .protocol_error(ce_protocol_error)
    );

    localparam int REQUEST_CLASSES = 4;
    localparam int SEQUENCE_WIDTH = 16;
    localparam int ADDRESS_WIDTH = 32;
    localparam int LENGTH_WIDTH = 8;
    logic arb_mode_batch_class;
    logic [REQUEST_CLASSES-1:0] arb_s_valid;
    logic [REQUEST_CLASSES-1:0] arb_s_ready;
    logic [REQUEST_CLASSES*BATCH_ID_WIDTH-1:0] arb_s_batch_id;
    logic [REQUEST_CLASSES*SEQUENCE_WIDTH-1:0] arb_s_sequence;
    logic [REQUEST_CLASSES*ADDRESS_WIDTH-1:0] arb_s_address;
    logic [REQUEST_CLASSES*LENGTH_WIDTH-1:0] arb_s_length;
    logic [REQUEST_CLASSES-1:0] arb_s_write;
    logic arb_m_valid;
    logic arb_m_ready;
    logic [1:0] arb_m_class;
    logic [BATCH_ID_WIDTH-1:0] arb_m_batch_id;
    logic [SEQUENCE_WIDTH-1:0] arb_m_sequence;
    logic [ADDRESS_WIDTH-1:0] arb_m_address;
    logic [LENGTH_WIDTH-1:0] arb_m_length;
    logic arb_m_write;

    hygcn_request_arbiter #(
        .REQUEST_CLASSES(REQUEST_CLASSES),
        .BATCH_ID_WIDTH(BATCH_ID_WIDTH),
        .SEQUENCE_WIDTH(SEQUENCE_WIDTH),
        .ADDRESS_WIDTH(ADDRESS_WIDTH),
        .LENGTH_WIDTH(LENGTH_WIDTH)
    ) request_arbiter (
        .mode_batch_class(arb_mode_batch_class),
        .s_valid(arb_s_valid),
        .s_ready(arb_s_ready),
        .s_batch_id(arb_s_batch_id),
        .s_sequence(arb_s_sequence),
        .s_address(arb_s_address),
        .s_length(arb_s_length),
        .s_write(arb_s_write),
        .m_valid(arb_m_valid),
        .m_ready(arb_m_ready),
        .m_class(arb_m_class),
        .m_batch_id(arb_m_batch_id),
        .m_sequence(arb_m_sequence),
        .m_address(arb_m_address),
        .m_length(arb_m_length),
        .m_write(arb_m_write)
    );

    logic buffer_alloc_valid;
    logic buffer_alloc_ready;
    logic [BATCH_ID_WIDTH-1:0] buffer_alloc_batch_id;
    logic [15:0] buffer_alloc_bytes;
    logic buffer_alloc_bank_id;
    logic buffer_ae_done_valid;
    logic buffer_ae_done_bank_id;
    logic buffer_ce_valid;
    logic buffer_ce_ready;
    logic buffer_ce_bank_id;
    logic [BATCH_ID_WIDTH-1:0] buffer_ce_batch_id;
    logic [15:0] buffer_ce_bytes;
    logic buffer_ce_done_valid;
    logic buffer_ce_done_bank_id;
    logic [5:0] buffer_bank_state;
    logic buffer_protocol_error;

    hygcn_aggregation_buffer_ctrl #(
        .BANKS(2),
        .BANK_BYTES(1024),
        .BATCH_ID_WIDTH(BATCH_ID_WIDTH),
        .BYTE_COUNT_WIDTH(16)
    ) aggregation_buffer_ctrl (
        .clk,
        .rst_n,
        .alloc_valid(buffer_alloc_valid),
        .alloc_ready(buffer_alloc_ready),
        .alloc_batch_id(buffer_alloc_batch_id),
        .alloc_bytes(buffer_alloc_bytes),
        .alloc_bank_id(buffer_alloc_bank_id),
        .ae_done_valid(buffer_ae_done_valid),
        .ae_done_bank_id(buffer_ae_done_bank_id),
        .ce_valid(buffer_ce_valid),
        .ce_ready(buffer_ce_ready),
        .ce_bank_id(buffer_ce_bank_id),
        .ce_batch_id(buffer_ce_batch_id),
        .ce_bytes(buffer_ce_bytes),
        .ce_done_valid(buffer_ce_done_valid),
        .ce_done_bank_id(buffer_ce_done_bank_id),
        .bank_state(buffer_bank_state),
        .protocol_error(buffer_protocol_error)
    );

    task automatic cycle;
        @(posedge clk);
        #1;
    endtask

    task automatic check(input logic condition, input string message);
        if (!condition) begin
            $fatal(1, "CHECK FAILED: %s", message);
        end
    endtask

    task automatic set_agg_vector(
        input integer lane0,
        input integer lane1,
        input integer lane2,
        input integer lane3
    );
        agg_s_data[0*DATA_WIDTH +: DATA_WIDTH] = lane0[DATA_WIDTH-1:0];
        agg_s_data[1*DATA_WIDTH +: DATA_WIDTH] = lane1[DATA_WIDTH-1:0];
        agg_s_data[2*DATA_WIDTH +: DATA_WIDTH] = lane2[DATA_WIDTH-1:0];
        agg_s_data[3*DATA_WIDTH +: DATA_WIDTH] = lane3[DATA_WIDTH-1:0];
    endtask

    task automatic set_ce_activation(
        input integer lane0,
        input integer lane1,
        input integer lane2,
        input integer lane3
    );
        ce_s_activation[0*DATA_WIDTH +: DATA_WIDTH] = lane0[DATA_WIDTH-1:0];
        ce_s_activation[1*DATA_WIDTH +: DATA_WIDTH] = lane1[DATA_WIDTH-1:0];
        ce_s_activation[2*DATA_WIDTH +: DATA_WIDTH] = lane2[DATA_WIDTH-1:0];
        ce_s_activation[3*DATA_WIDTH +: DATA_WIDTH] = lane3[DATA_WIDTH-1:0];
    endtask

    task automatic set_cluster_vector(
        input integer core_index,
        input integer lane0,
        input integer lane1,
        input integer lane2,
        input integer lane3
    );
        cluster_s_data[(core_index*LANES+0)*DATA_WIDTH +: DATA_WIDTH] =
            lane0[DATA_WIDTH-1:0];
        cluster_s_data[(core_index*LANES+1)*DATA_WIDTH +: DATA_WIDTH] =
            lane1[DATA_WIDTH-1:0];
        cluster_s_data[(core_index*LANES+2)*DATA_WIDTH +: DATA_WIDTH] =
            lane2[DATA_WIDTH-1:0];
        cluster_s_data[(core_index*LANES+3)*DATA_WIDTH +: DATA_WIDTH] =
            lane3[DATA_WIDTH-1:0];
    endtask

    task automatic set_ce_weights(
        input integer output_index,
        input integer lane0,
        input integer lane1,
        input integer lane2,
        input integer lane3
    );
        ce_s_weight[(output_index*LANES+0)*DATA_WIDTH +: DATA_WIDTH] = lane0[DATA_WIDTH-1:0];
        ce_s_weight[(output_index*LANES+1)*DATA_WIDTH +: DATA_WIDTH] = lane1[DATA_WIDTH-1:0];
        ce_s_weight[(output_index*LANES+2)*DATA_WIDTH +: DATA_WIDTH] = lane2[DATA_WIDTH-1:0];
        ce_s_weight[(output_index*LANES+3)*DATA_WIDTH +: DATA_WIDTH] = lane3[DATA_WIDTH-1:0];
    endtask

    task automatic drive_aggregation(
        input logic first_beat,
        input logic last_beat,
        input logic max_operation
    );
        agg_s_first = first_beat;
        agg_s_last = last_beat;
        agg_s_op_max = max_operation;
        agg_s_valid = 1'b1;
        while (!agg_s_ready) begin
            cycle();
        end
        cycle();
        agg_s_valid = 1'b0;
    endtask

    task automatic drive_combination(
        input logic first_beat,
        input logic last_beat
    );
        ce_s_first = first_beat;
        ce_s_last = last_beat;
        ce_s_valid = 1'b1;
        while (!ce_s_ready) begin
            cycle();
        end
        cycle();
        ce_s_valid = 1'b0;
    endtask

    integer index;
    initial begin
        agg_s_valid = 1'b0;
        agg_s_batch_id = '0;
        agg_s_vertex_id = '0;
        agg_s_first = 1'b0;
        agg_s_last = 1'b0;
        agg_s_op_max = 1'b0;
        agg_s_data = '0;
        agg_m_ready = 1'b0;

        cluster_s_valid = '0;
        cluster_s_batch_id = '0;
        cluster_s_vertex_id = '0;
        cluster_s_first = '0;
        cluster_s_last = '0;
        cluster_s_op_max = '0;
        cluster_s_data = '0;
        cluster_m_ready = '0;

        ce_s_valid = 1'b0;
        ce_s_batch_id = '0;
        ce_s_vertex_id = '0;
        ce_s_first = 1'b0;
        ce_s_last = 1'b0;
        ce_s_activation = '0;
        ce_s_weight = '0;
        ce_s_bias = '0;
        ce_m_ready = 1'b0;

        arb_mode_batch_class = 1'b0;
        arb_s_valid = '0;
        arb_s_batch_id = '0;
        arb_s_sequence = '0;
        arb_s_address = '0;
        arb_s_length = '0;
        arb_s_write = '0;
        arb_m_ready = 1'b1;

        buffer_alloc_valid = 1'b0;
        buffer_alloc_batch_id = '0;
        buffer_alloc_bytes = '0;
        buffer_ae_done_valid = 1'b0;
        buffer_ae_done_bank_id = '0;
        buffer_ce_ready = 1'b0;
        buffer_ce_done_valid = 1'b0;
        buffer_ce_done_bank_id = '0;

        repeat (3) cycle();
        rst_n = 1'b1;
        cycle();

        // SUM aggregation and output backpressure.
        agg_s_batch_id = 8'd3;
        agg_s_vertex_id = 16'd9;
        set_agg_vector(1, 2, 3, 4);
        drive_aggregation(1'b1, 1'b0, 1'b0);
        set_agg_vector(10, 20, 30, 40);
        drive_aggregation(1'b0, 1'b1, 1'b0);
        check(agg_m_valid, "SUM result must be valid");
        check(agg_m_batch_id == 3 && agg_m_vertex_id == 9,
              "SUM metadata must follow the reduction");
        check(!agg_s_ready, "aggregation input must stop under output backpressure");
        check($signed(agg_m_data[0*ACC_WIDTH +: ACC_WIDTH]) == 11, "SUM lane 0");
        check($signed(agg_m_data[1*ACC_WIDTH +: ACC_WIDTH]) == 22, "SUM lane 1");
        check($signed(agg_m_data[2*ACC_WIDTH +: ACC_WIDTH]) == 33, "SUM lane 2");
        check($signed(agg_m_data[3*ACC_WIDTH +: ACC_WIDTH]) == 44, "SUM lane 3");
        cycle();
        check($signed(agg_m_data[3*ACC_WIDTH +: ACC_WIDTH]) == 44,
              "aggregation output must remain stable while stalled");
        agg_m_ready = 1'b1;
        cycle();
        agg_m_ready = 1'b0;
        check(!agg_m_valid, "SUM result must be consumed");

        // MAX aggregation with negative signed data.
        agg_s_batch_id = 8'd4;
        agg_s_vertex_id = 16'd10;
        set_agg_vector(-5, 7, 2, -1);
        drive_aggregation(1'b1, 1'b0, 1'b1);
        set_agg_vector(-2, 3, 9, -8);
        drive_aggregation(1'b0, 1'b1, 1'b1);
        check($signed(agg_m_data[0*ACC_WIDTH +: ACC_WIDTH]) == -2, "MAX lane 0");
        check($signed(agg_m_data[1*ACC_WIDTH +: ACC_WIDTH]) == 7, "MAX lane 1");
        check($signed(agg_m_data[2*ACC_WIDTH +: ACC_WIDTH]) == 9, "MAX lane 2");
        check($signed(agg_m_data[3*ACC_WIDTH +: ACC_WIDTH]) == -1, "MAX lane 3");
        agg_m_ready = 1'b1;
        cycle();
        agg_m_ready = 1'b0;
        check(!agg_protocol_error, "legal aggregation stream must not set protocol_error");

        // Two aggregation cores accept and retire independent vertices in parallel.
        cluster_s_batch_id[0*BATCH_ID_WIDTH +: BATCH_ID_WIDTH] = 8'd20;
        cluster_s_batch_id[1*BATCH_ID_WIDTH +: BATCH_ID_WIDTH] = 8'd21;
        cluster_s_vertex_id[0*VERTEX_ID_WIDTH +: VERTEX_ID_WIDTH] = 16'd30;
        cluster_s_vertex_id[1*VERTEX_ID_WIDTH +: VERTEX_ID_WIDTH] = 16'd31;
        cluster_s_op_max = 2'b10;
        cluster_s_first = 2'b11;
        cluster_s_last = 2'b00;
        set_cluster_vector(0, 1, 2, 3, 4);
        set_cluster_vector(1, -5, 7, 2, -1);
        cluster_s_valid = 2'b11;
        cycle();
        cluster_s_first = 2'b00;
        cluster_s_last = 2'b11;
        set_cluster_vector(0, 10, 20, 30, 40);
        set_cluster_vector(1, -2, 3, 9, -8);
        cycle();
        cluster_s_valid = '0;
        check(cluster_m_valid == 2'b11, "aggregation cores must retire in parallel");
        check(cluster_m_batch_id[0*BATCH_ID_WIDTH +: BATCH_ID_WIDTH] == 20 &&
              cluster_m_batch_id[1*BATCH_ID_WIDTH +: BATCH_ID_WIDTH] == 21,
              "parallel aggregation metadata");
        check($signed(cluster_m_data[(0*LANES+3)*ACC_WIDTH +: ACC_WIDTH]) == 44,
              "parallel SUM core result");
        check($signed(cluster_m_data[(1*LANES+2)*ACC_WIDTH +: ACC_WIDTH]) == 9,
              "parallel MAX core result");
        cluster_m_ready = 2'b11;
        cycle();
        cluster_m_ready = '0;
        check(cluster_protocol_error == '0, "parallel aggregation streams must be legal");

        // Four parallel combination arrays over two inner beats.
        ce_s_batch_id = 8'd7;
        ce_s_vertex_id = 16'd21;
        ce_s_bias = '0;
        ce_s_bias[0*ACC_WIDTH +: ACC_WIDTH] = 1;
        ce_s_bias[1*ACC_WIDTH +: ACC_WIDTH] = 2;
        ce_s_bias[2*ACC_WIDTH +: ACC_WIDTH] = 3;
        ce_s_bias[3*ACC_WIDTH +: ACC_WIDTH] = 4;
        set_ce_activation(1, 2, 3, 4);
        set_ce_weights(0, 1, 1, 1, 1);
        set_ce_weights(1, 1, 0, -1, 2);
        set_ce_weights(2, -1, -1, -1, -1);
        set_ce_weights(3, 2, 2, 2, 2);
        drive_combination(1'b1, 1'b0);
        set_ce_activation(1, 1, 1, 1);
        for (index = 0; index < OUTPUTS; index = index + 1) begin
            set_ce_weights(index, 1, 1, 1, 1);
        end
        drive_combination(1'b0, 1'b1);
        check(ce_m_valid, "combination result must be valid");
        check(ce_m_batch_id == 7 && ce_m_vertex_id == 21,
              "combination metadata must follow the tile");
        check($signed(ce_m_data[0*ACC_WIDTH +: ACC_WIDTH]) == 15, "CE output 0");
        check($signed(ce_m_data[1*ACC_WIDTH +: ACC_WIDTH]) == 12, "CE output 1");
        check($signed(ce_m_data[2*ACC_WIDTH +: ACC_WIDTH]) == -3, "CE output 2");
        check($signed(ce_m_data[3*ACC_WIDTH +: ACC_WIDTH]) == 28, "CE output 3");
        ce_m_ready = 1'b1;
        cycle();
        ce_m_ready = 1'b0;
        check(!ce_protocol_error, "legal combination stream must not set protocol_error");

        // Product width must retain values beyond the signed 16-bit range.
        ce_s_bias = '0;
        set_ce_activation(30000, 30000, 30000, 30000);
        for (index = 0; index < OUTPUTS; index = index + 1) begin
            set_ce_weights(index, 2, 2, 2, 2);
        end
        drive_combination(1'b1, 1'b1);
        for (index = 0; index < OUTPUTS; index = index + 1) begin
            check($signed(ce_m_data[index*ACC_WIDTH +: ACC_WIDTH]) == 240000,
                  "CE product width must not truncate");
        end
        ce_m_ready = 1'b1;
        cycle();
        ce_m_ready = 1'b0;

        // FIFO chooses the oldest sequence; batch-class chooses batch then class.
        arb_s_valid = 4'b1111;
        arb_s_batch_id[0*BATCH_ID_WIDTH +: BATCH_ID_WIDTH] = 2;
        arb_s_batch_id[1*BATCH_ID_WIDTH +: BATCH_ID_WIDTH] = 1;
        arb_s_batch_id[2*BATCH_ID_WIDTH +: BATCH_ID_WIDTH] = 1;
        arb_s_batch_id[3*BATCH_ID_WIDTH +: BATCH_ID_WIDTH] = 0;
        arb_s_sequence[0*SEQUENCE_WIDTH +: SEQUENCE_WIDTH] = 1;
        arb_s_sequence[1*SEQUENCE_WIDTH +: SEQUENCE_WIDTH] = 100;
        arb_s_sequence[2*SEQUENCE_WIDTH +: SEQUENCE_WIDTH] = 50;
        arb_s_sequence[3*SEQUENCE_WIDTH +: SEQUENCE_WIDTH] = 200;
        arb_s_address[0*ADDRESS_WIDTH +: ADDRESS_WIDTH] = 32'h1000;
        arb_s_address[1*ADDRESS_WIDTH +: ADDRESS_WIDTH] = 32'h2000;
        arb_s_address[2*ADDRESS_WIDTH +: ADDRESS_WIDTH] = 32'h3000;
        arb_s_address[3*ADDRESS_WIDTH +: ADDRESS_WIDTH] = 32'h4000;
        arb_s_length[0*LENGTH_WIDTH +: LENGTH_WIDTH] = 8'd1;
        arb_s_length[1*LENGTH_WIDTH +: LENGTH_WIDTH] = 8'd2;
        arb_s_length[2*LENGTH_WIDTH +: LENGTH_WIDTH] = 8'd3;
        arb_s_length[3*LENGTH_WIDTH +: LENGTH_WIDTH] = 8'd4;
        arb_s_write = 4'b1000;
        #1;
        check(arb_m_valid && arb_m_class == 0, "FIFO must select oldest sequence");
        check(arb_m_sequence == 1 && arb_m_address == 32'h1000 &&
              arb_m_length == 1 && !arb_m_write,
              "FIFO payload must match the selected request");
        check(arb_s_ready == 4'b0001, "FIFO ready must be one-hot");
        arb_mode_batch_class = 1'b1;
        #1;
        check(arb_m_class == 3 && arb_m_batch_id == 0,
              "batch-class must select earliest batch");
        check(arb_m_address == 32'h4000 && arb_m_length == 4 && arb_m_write,
              "batch-class payload must match the selected request");
        arb_s_valid[3] = 1'b0;
        #1;
        check(arb_m_class == 1 && arb_m_batch_id == 1,
              "same batch must select Edge/Input/Weight/Output class order");
        arb_s_valid = '0;

        // Ping-pong buffer: two active banks, ready-to-CE, completion and reuse.
        buffer_alloc_batch_id = 10;
        buffer_alloc_bytes = 16'd512;
        buffer_alloc_valid = 1'b1;
        #1;
        check(buffer_alloc_ready && buffer_alloc_bank_id == 0,
              "first allocation must use bank 0");
        cycle();
        buffer_alloc_batch_id = 11;
        #1;
        check(buffer_alloc_ready && buffer_alloc_bank_id == 1,
              "second allocation must use bank 1");
        cycle();
        check(!buffer_alloc_ready, "both active banks must exert allocation backpressure");
        buffer_alloc_valid = 1'b0;

        buffer_ae_done_bank_id = 1'b1;
        buffer_ae_done_valid = 1'b1;
        cycle();
        buffer_ae_done_valid = 1'b0;
        check(buffer_ce_valid && buffer_ce_bank_id == 1 && buffer_ce_batch_id == 11 &&
              buffer_ce_bytes == 512,
              "CE may launch only an AE-ready bank");
        buffer_ce_ready = 1'b1;
        cycle();
        buffer_ce_ready = 1'b0;
        buffer_ce_done_bank_id = 1'b1;
        buffer_ce_done_valid = 1'b1;
        cycle();
        buffer_ce_done_valid = 1'b0;
        buffer_alloc_batch_id = 12;
        buffer_alloc_bytes = 16'd256;
        buffer_alloc_valid = 1'b1;
        #1;
        check(buffer_alloc_ready && buffer_alloc_bank_id == 1,
              "completed CE bank must be reusable");
        cycle();
        buffer_alloc_valid = 1'b0;
        check(!buffer_protocol_error, "legal buffer lifecycle must not set protocol_error");

        // Malformed transactions must be observable instead of silently accepted.
        agg_s_batch_id = 8'd99;
        agg_s_vertex_id = 16'd99;
        set_agg_vector(1, 1, 1, 1);
        drive_aggregation(1'b0, 1'b1, 1'b0);
        check(agg_protocol_error, "aggregation protocol error must be sticky");

        ce_s_batch_id = 8'd99;
        ce_s_vertex_id = 16'd99;
        set_ce_activation(1, 1, 1, 1);
        for (index = 0; index < OUTPUTS; index = index + 1) begin
            set_ce_weights(index, 1, 1, 1, 1);
        end
        drive_combination(1'b0, 1'b1);
        check(ce_protocol_error, "combination protocol error must be sticky");

        buffer_alloc_batch_id = 8'd99;
        buffer_alloc_bytes = 16'd2048;
        buffer_alloc_valid = 1'b1;
        cycle();
        buffer_alloc_valid = 1'b0;
        check(buffer_protocol_error, "oversized buffer allocation must be rejected");

        $display("HYGCN_RTL_TESTS_PASS");
        $finish;
    end
endmodule

`default_nettype wire
