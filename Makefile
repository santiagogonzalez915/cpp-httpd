CXX       = g++
CXXFLAGS  = -std=c++17 -Wall -Wextra -I. -Ihttp
BUILD_DIR = build

# OpenSSL — brew install openssl (already present on most macOS systems via Homebrew)
OPENSSL_CFLAGS = $(shell pkg-config --cflags openssl 2>/dev/null || \
                          echo -I/opt/homebrew/opt/openssl/include)
OPENSSL_LIBS   = $(shell pkg-config --libs   openssl 2>/dev/null || \
                          echo -L/opt/homebrew/opt/openssl/lib -lssl -lcrypto)

SERVER_OBJS = \
	server/server.o server/socket_setup.o server/management_thread.o \
	server/thread_pool_mode.o server/select_mode.o server/tls.o \
	config/config.o \
	http/core/http_request.o http/core/request_parser.o http/core/http_response.o http/core/request_handler_utils.o \
	http/handlers/request_handler.o \
	http/connection/connection_manager.o http/routing/vhost_resolver.o \
	http/handlers/auth_handler.o http/handlers/static_file_handler.o http/handlers/directory_handler.o http/handlers/cgi_handler.o

all: $(BUILD_DIR) main.cpp $(SERVER_OBJS)
	$(CXX) $(CXXFLAGS) $(OPENSSL_CFLAGS) -o $(BUILD_DIR)/server_bin main.cpp $(SERVER_OBJS) $(OPENSSL_LIBS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(OPENSSL_CFLAGS) -c -o $@ $<

http/core/request_parser.o: http/core/request_parser.hpp http/core/http_request.hpp
http/handlers/request_handler.o: http/handlers/request_handler.hpp http/core/request_handler_utils.hpp http/core/phase_context.hpp config/config.hpp http/core/http_request.hpp http/core/http_response.hpp
http/connection/connection_manager.o: http/connection/connection_manager.hpp http/core/request_parser.hpp http/handlers/request_handler.hpp http/core/http_request.hpp http/core/http_response.hpp http/core/request_handler_utils.hpp
http/routing/vhost_resolver.o: http/routing/vhost_resolver.hpp http/core/request_handler_utils.hpp http/core/http_request.hpp config/config.hpp
http/handlers/auth_handler.o: http/handlers/auth_handler.hpp http/core/auth_result.hpp http/core/request_handler_utils.hpp http/core/http_request.hpp
http/handlers/static_file_handler.o: http/handlers/static_file_handler.hpp http/core/request_handler_utils.hpp http/core/http_request.hpp http/core/http_response.hpp
http/handlers/directory_handler.o: http/handlers/directory_handler.hpp http/handlers/static_file_handler.hpp http/core/request_handler_utils.hpp http/core/http_request.hpp http/core/http_response.hpp
http/handlers/cgi_handler.o: http/handlers/cgi_handler.hpp http/core/request_handler_utils.hpp http/core/http_request.hpp http/core/http_response.hpp config/config.hpp
server/management_thread.o: server/management_thread.hpp http/core/request_handler_utils.hpp
server/server.o: server/server.hpp http/handlers/request_handler.hpp server/management_thread.hpp server/socket_setup.hpp config/config.hpp
server/thread_pool_mode.o: server/server.hpp server/management_thread.hpp http/connection/connection_manager.hpp http/handlers/request_handler.hpp config/config.hpp
server/select_mode.o: server/server.hpp server/management_thread.hpp http/connection/connection_manager.hpp http/core/http_request.hpp http/core/http_response.hpp http/handlers/request_handler.hpp config/config.hpp

SERVER_SRCS = \
	server/server.cpp server/socket_setup.cpp server/management_thread.cpp \
	server/thread_pool_mode.cpp server/select_mode.cpp server/tls.cpp \
	config/config.cpp \
	http/core/http_request.cpp http/core/request_parser.cpp http/core/http_response.cpp http/core/request_handler_utils.cpp \
	http/handlers/request_handler.cpp \
	http/connection/connection_manager.cpp http/routing/vhost_resolver.cpp \
	http/handlers/auth_handler.cpp http/handlers/static_file_handler.cpp http/handlers/directory_handler.cpp http/handlers/cgi_handler.cpp

# Single-compilation sanitizer builds — all TUs must share the same flags.
asan: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OPENSSL_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o $(BUILD_DIR)/server_bin_asan main.cpp $(SERVER_SRCS) $(OPENSSL_LIBS)

ubsan: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OPENSSL_CFLAGS) -fsanitize=undefined \
		-o $(BUILD_DIR)/server_bin_ubsan main.cpp $(SERVER_SRCS) $(OPENSSL_LIBS)

TEST_SRCS = \
	tests/test_request_parser.cpp \
	tests/test_http_response.cpp

CATCH2_CFLAGS = $(shell pkg-config --cflags catch2-with-main)
CATCH2_LIBS   = $(shell pkg-config --libs   catch2-with-main)

# Unit test suite — requires Catch2: brew install catch2
test: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CATCH2_CFLAGS) \
		$(TEST_SRCS) \
		http/core/request_parser.cpp http/core/http_request.cpp \
		http/core/http_response.cpp  http/core/request_handler_utils.cpp \
		$(CATCH2_LIBS) -o $(BUILD_DIR)/test_runner
	$(BUILD_DIR)/test_runner

FUZZ_SRCS = \
	http/core/request_parser.cpp \
	http/core/http_request.cpp \
	http/core/request_handler_utils.cpp

# Standalone ASAN driver — works with Apple clang, compatible with AFL++.
# Usage: ./build/fuzz_driver < fuzz/corpus/get_basic.txt
fuzz_driver: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o $(BUILD_DIR)/fuzz_driver \
		fuzz/fuzz_driver.cpp $(FUZZ_SRCS)

# libFuzzer target — requires LLVM from Homebrew: brew install llvm
# Then run: ./build/fuzz_parser fuzz/corpus/ -max_total_time=300
LLVM_CLANG = $(shell ls /opt/homebrew/opt/llvm/bin/clang++ 2>/dev/null || \
                      ls /usr/local/opt/llvm/bin/clang++ 2>/dev/null || echo clang++)
fuzz_parser: $(BUILD_DIR)
	$(LLVM_CLANG) -std=c++17 -I. -Ihttp \
		-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
		-o $(BUILD_DIR)/fuzz_parser \
		fuzz/fuzz_request_parser.cpp $(FUZZ_SRCS)

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(SERVER_OBJS)

.PHONY: all clean asan ubsan test fuzz_driver fuzz_parser
