CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -I.

BUILD_DIR = build

DBMS_OBJS = $(BUILD_DIR)/main.o $(BUILD_DIR)/src/parser.o
TEST_OBJS = $(BUILD_DIR)/tests/test_parser.o $(BUILD_DIR)/src/parser.o

dbms: $(DBMS_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: dbms
	./dbms

test: $(BUILD_DIR)/run_tests
	$<

$(BUILD_DIR)/run_tests: $(TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -rf $(BUILD_DIR) dbms

.PHONY: run test clean
