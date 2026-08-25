default:
	$(CXX) -std=c++17 -fsanitize=address Query.cpp -o Query
	./Query
	$(CXX) -std=c++17 -fsanitize=address ChunkedEncoding.cpp -o ChunkedEncoding
	./ChunkedEncoding
	$(CXX) -std=c++17 -fsanitize=address TopicTree.cpp -o TopicTree
	./TopicTree
	$(CXX) -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG -std=c++17 -g -fsanitize=address HttpRouter.cpp -o HttpRouter
	./HttpRouter
	$(CXX) -std=c++17 -fsanitize=address BloomFilter.cpp -o BloomFilter
	./BloomFilter
	$(CXX) -std=c++17 -fsanitize=address ExtensionsNegotiator.cpp -o ExtensionsNegotiator
	./ExtensionsNegotiator
	$(CXX) -std=c++17 -fsanitize=address HttpParser.cpp -o HttpParser
	./HttpParser
	$(CXX) -std=c++17 -fsanitize=address -I../uSockets/src CoAkkaHttp1.cpp -o CoAkkaHttp1
	./CoAkkaHttp1

coakka-http1:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -I../uSockets/src CoAkkaHttp1.cpp -o CoAkkaHttp1.strict
	./CoAkkaHttp1.strict

coakka-http1-ubsan:
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsanitize=undefined -fno-sanitize-recover=undefined -I../uSockets/src CoAkkaHttp1.cpp -o CoAkkaHttp1.ubsan
	UBSAN_OPTIONS=halt_on_error=1 ./CoAkkaHttp1.ubsan

coakka-http1-context:
	$(MAKE) -C ../uSockets WITH_LIBUV=1 WITH_LTO=0 CFLAGS="$$(pkg-config --cflags libuv)"
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread -DLIBUS_NO_SSL -DLIBUS_USE_LIBUV -DUWS_NO_ZLIB -I../uSockets/src $$(pkg-config --cflags libuv) CoAkkaHttp1Context.cpp ../uSockets/uSockets.a $$(pkg-config --libs libuv) -o CoAkkaHttp1Context
	./CoAkkaHttp1Context

coakka-http1-context-ubsan:
	$(MAKE) -C ../uSockets WITH_LIBUV=1 WITH_LTO=0 CFLAGS="$$(pkg-config --cflags libuv) -fsanitize=undefined -fno-sanitize-recover=undefined"
	$(CXX) -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread -DLIBUS_NO_SSL -DLIBUS_USE_LIBUV -DUWS_NO_ZLIB -fsanitize=undefined -fno-sanitize-recover=undefined -I../uSockets/src $$(pkg-config --cflags libuv) CoAkkaHttp1Context.cpp ../uSockets/uSockets.a $$(pkg-config --libs libuv) -o CoAkkaHttp1Context.ubsan
	UBSAN_OPTIONS=halt_on_error=1 ./CoAkkaHttp1Context.ubsan

performance:
	$(CXX) -std=c++17 HttpRouter.cpp -O3 -o HttpRouter
	./HttpRouter

smoke:
	../Crc32 &
	sleep 1
	~/.deno/bin/deno run --allow-net smoke.mjs
	node smoke.mjs
	pkill Crc32

compliance:
	../EchoBody &
	sleep 1
	~/.deno/bin/deno run --allow-net ../h1spec/http_test.ts localhost 3000
	pkill EchoBody
