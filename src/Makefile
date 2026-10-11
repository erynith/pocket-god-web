SRC = vm/dex.c vm/heap.c vm/interp.c vm/jlang.c vm/jutil.c vm/jio.c vm/xml.c vm/android.c
WEBSRC = $(SRC) vm/vorbis.c
CFLAGS_COMMON = -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers

harness/pgvm: $(SRC) vm/*.h harness/main.c
	gcc $(CFLAGS_COMMON) -O1 -g -fsanitize=address,undefined -fno-sanitize=alignment -o $@ $(SRC) harness/main.c -lm

harness/pgvm-fast: $(SRC) vm/*.h harness/main.c
	clang $(CFLAGS_COMMON) -O2 -g -o $@ $(SRC) harness/main.c -lm

web/pgvm.wasm: $(WEBSRC) vm/*.h
	clang --target=wasm32-wasi --sysroot=/usr $(CFLAGS_COMMON) -O2 -mbulk-memory -mnontrapping-fptoint \
	  -Wl,--no-entry -Wl,--export-dynamic -Wl,--allow-undefined -Wl,--initial-memory=67108864 -Wl,--max-memory=4294967296 -Wl,-z,stack-size=4194304 \
	  -Wl,--import-undefined -mexec-model=reactor -o $@ $(WEBSRC)
