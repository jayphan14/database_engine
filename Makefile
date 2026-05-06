CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -I.

BUILD_DIR = build

DBMS_OBJS = $(BUILD_DIR)/main.o \
            $(BUILD_DIR)/src/util/string.o \
            $(BUILD_DIR)/src/sql/parser.o \
            $(BUILD_DIR)/src/storage/disk_manager.o \
            $(BUILD_DIR)/src/storage/buffer_pool.o \
            $(BUILD_DIR)/src/storage/slotted_page.o \
            $(BUILD_DIR)/src/storage/heap_file.o \
            $(BUILD_DIR)/src/sql/tuple.o \
            $(BUILD_DIR)/src/sql/catalog.o \
            $(BUILD_DIR)/src/sql/analyzer.o \
            $(BUILD_DIR)/src/sql/plan_node.o \
            $(BUILD_DIR)/src/sql/operators.o \
            $(BUILD_DIR)/src/sql/planner.o \
            $(BUILD_DIR)/src/sql/executor.o
TEST_OBJS = $(BUILD_DIR)/tests/sql/test_parser.o \
            $(BUILD_DIR)/tests/storage/test_disk_manager.o \
            $(BUILD_DIR)/tests/storage/test_buffer_pool.o \
            $(BUILD_DIR)/tests/storage/test_slotted_page.o \
            $(BUILD_DIR)/tests/storage/test_heap_file.o \
            $(BUILD_DIR)/tests/storage/test_integration.o \
            $(BUILD_DIR)/tests/sql/test_tuple.o \
            $(BUILD_DIR)/tests/sql/test_catalog.o \
            $(BUILD_DIR)/tests/sql/test_analyzer.o \
            $(BUILD_DIR)/tests/sql/test_executor.o \
            $(BUILD_DIR)/tests/sql/test_operators.o \
            $(BUILD_DIR)/src/util/string.o \
            $(BUILD_DIR)/src/sql/parser.o \
            $(BUILD_DIR)/src/storage/disk_manager.o \
            $(BUILD_DIR)/src/storage/buffer_pool.o \
            $(BUILD_DIR)/src/storage/slotted_page.o \
            $(BUILD_DIR)/src/storage/heap_file.o \
            $(BUILD_DIR)/src/sql/tuple.o \
            $(BUILD_DIR)/src/sql/catalog.o \
            $(BUILD_DIR)/src/sql/analyzer.o \
            $(BUILD_DIR)/src/sql/plan_node.o \
            $(BUILD_DIR)/src/sql/operators.o \
            $(BUILD_DIR)/src/sql/planner.o \
            $(BUILD_DIR)/src/sql/executor.o

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
