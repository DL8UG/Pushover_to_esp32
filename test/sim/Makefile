# PC simulator for the e-paper UI.
#   make -C test/sim        render all screens to test/sim/build/*.png
#                           and refresh docs/img/screens.png + board.svg
ROOT  := ../..
MAIN  := $(ROOT)/main
BUILD := build
SRCS  := sim.cpp $(MAIN)/ui.cpp $(MAIN)/render.cpp $(MAIN)/fonts_data.cpp
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror -I$(MAIN)

all: $(BUILD)/sim
	rm -f $(BUILD)/*.pbm $(BUILD)/*.png
	$(BUILD)/sim $(BUILD)
	python3 $(ROOT)/tools/sim_png.py $(BUILD) $(ROOT)/docs/img/screens.png
	python3 $(ROOT)/tools/board_svg.py $(BUILD)/home-many.pbm $(ROOT)/docs/img/board.svg

$(BUILD)/sim: $(SRCS) $(wildcard $(MAIN)/*.h)
	mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
