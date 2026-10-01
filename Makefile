CXX      ?= g++
CXXFLAGS ?= -O2 -march=native -std=c++17
BIN      := bin

BINARIES := $(BIN)/run $(BIN)/run-b $(BIN)/run-both $(BIN)/trace_stats \
            $(BIN)/gen_zipf \
            $(BIN)/hwmodel_scan $(BIN)/letheb_hwmodel

.PHONY: all clean
all: $(BINARIES)

$(BIN):
	mkdir -p $(BIN)

# run and run-b are the same source. A binary carrying both stores loses the
# default a few percent of its update rate to code layout, so a build meant for
# measurement leaves one out; run-both carries both and is the convenient build
# for accuracy, where layout does not matter.
$(BIN)/run: cpu/run.cc | $(BIN)
	$(CXX) $(CXXFLAGS) -DLETHE_NO_PACKED -o $@ $<

$(BIN)/run-b: cpu/run.cc | $(BIN)
	$(CXX) $(CXXFLAGS) -DLETHE_NO_PLAIN -o $@ $<

$(BIN)/run-both: cpu/run.cc | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BIN)/%: cpu/%.cc | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(BIN)/%: fpga/%.cc | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -rf $(BIN)
