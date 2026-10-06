ROOT := ..
BUILD_DIR ?= $(ROOT)/build-rtl
TOOL_ROOT ?= $(HOME)/.cache/conda-envs/hygcn-rtl
VERILATOR ?= $(TOOL_ROOT)/bin/verilator
YOSYS ?= $(TOOL_ROOT)/bin/yowasp-yosys

RTL_SOURCES := \
	hygcn_aggregation_engine.sv \
	hygcn_aggregation_cluster.sv \
	hygcn_combination_cluster.sv \
	hygcn_request_arbiter.sv \
	hygcn_aggregation_buffer_ctrl.sv

.PHONY: all lint sim synth clean

all: lint sim synth

lint:
	$(VERILATOR) --lint-only --timing -Wall -Wno-fatal \
		-Wno-UNUSEDSIGNAL -Wno-BLKSEQ \
		$(RTL_SOURCES) tb/tb_hygcn_rtl.sv --top-module tb_hygcn_rtl
	$(VERILATOR) --lint-only -Wall -Wno-fatal -Wno-UNUSEDSIGNAL \
		hygcn_aggregation_engine.sv hygcn_aggregation_cluster.sv \
		--top-module hygcn_aggregation_cluster
	$(VERILATOR) --lint-only -Wall -Wno-fatal -Wno-UNUSEDSIGNAL \
		hygcn_combination_cluster.sv --top-module hygcn_combination_cluster

sim:
	mkdir -p $(BUILD_DIR)
	$(VERILATOR) --binary --timing -Wall -Wno-fatal \
		-Wno-UNUSEDSIGNAL -Wno-BLKSEQ \
		$(RTL_SOURCES) tb/tb_hygcn_rtl.sv --top-module tb_hygcn_rtl \
		--Mdir $(BUILD_DIR)/obj_dir
	$(BUILD_DIR)/obj_dir/Vtb_hygcn_rtl

synth:
	mkdir -p $(BUILD_DIR)
	cd $(ROOT) && $(YOSYS) -s rtl/synth/smoke.ys \
		| tee build-rtl/yosys-smoke.log

clean:
	rm -rf $(BUILD_DIR)
