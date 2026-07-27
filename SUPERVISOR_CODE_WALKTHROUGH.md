# Memory Controller: Complete Supervisor Walkthrough

This document is a speaking guide for explaining the complete project workflow to a supervisor. It follows the current implementation from program startup to request completion and points to the exact source location for every code example.

> Line references in this document match the current source snapshot. If source files are edited, verify the references again before presenting.

## 1. Executive explanation

The project has two layers. The C layer is the software framework: it validates command-line options, loads an optional ROM image, parses a CSV file into an array of requests, invokes the simulation, prints statistics, and releases dynamic memory. The C++/SystemC layer is the hardware model: it creates signals and the memory hierarchy, drives requests clock by clock, routes each request through the memory controller, models latency and handshakes, and returns read values and error statistics. The separation is visible in the source lists and language-specific compilation rules. [Source: `Makefile:10-20`](Makefile#L10-L20)

The complete data path is:

```text
Command-line arguments and files
              |
              v
            main.c
       /                  \
ROM text file          Request CSV
       |                  |
       v                  v
 rom_loader.c         csv_parser.c
       |                  |
 uint32_t words       Request[] array
       \                  /
        v                v
             runSimulation()
                    |
                    v
              MEMORY_SYSTEM
             /             \
            v               v
 MEMORY_CONTROLLER <----> MAIN_MEMORY
            |
            v
   read data, ready, error
            |
            v
 Result { cycles, errors }
```

The C-to-SystemC call is made in `main()`, the SystemC hierarchy is instantiated in `runSimulation()`, and the hierarchy connects the controller to main memory. [Sources: `src/main.c:121-124`](src/main.c#L121-L124), [`src/simulation.cpp:12-44`](src/simulation.cpp#L12-L44), [`include/memory_system.hpp:31-74`](include/memory_system.hpp#L31-L74)

### A strong 60-second explanation

> My program first validates the simulation configuration and converts two text inputs into typed memory structures: the ROM file becomes an array of 32-bit words, and the CSV file becomes an array of `Request` structures. `runSimulation` bridges the C framework and SystemC model. It drives one request at a time into a hierarchical `MEMORY_SYSTEM`, which contains a `MEMORY_CONTROLLER` and `MAIN_MEMORY`. At a clock edge, the controller snapshots the request, lowers `ready`, validates the operation and address range, and selects ROM or RAM by comparing the address with `romSize`. ROM reads use an internal byte vector and wait for configurable ROM latency; ROM writes fail. RAM accesses first pass block-based ownership checks, then use a four-byte main-memory interface. One-byte reads mask the least-significant byte, and one-byte writes use read-modify-write. The controller waits for internal `mem_ready`, then raises external `ready`, returns read data, or raises `error`. The simulation stores read results back into the request array and returns total cycles and errors.

The described controller phases are implemented in `behavior()`, the RAM handshakes are implemented in `memoryRead()` and `memoryWrite()`, and simulation results are collected in `runSimulation()`. [Sources: `src/memory_controller.cpp:31-88`](src/memory_controller.cpp#L31-L88), [`src/memory_controller.cpp:237-272`](src/memory_controller.cpp#L237-L272), [`src/simulation.cpp:75-123`](src/simulation.cpp#L75-L123)

## 2. Build model and language boundary

The assignment contains both C and C++. The framework files are compiled as C17, while SystemC files are compiled as C++14. This matters because compiling C allocation and pointer code as C++ can create misleading type-conversion errors. The build explicitly uses `gcc` for `.c`, `g++` for `.cpp`, and `g++` for final SystemC linking. [Sources: `Makefile:1-13`](Makefile#L1-L13), [`Makefile:29-33`](Makefile#L29-L33)

```make
CFLAGS := -std=c17 -Wall -Wextra -O2 -Iinclude
CXXFLAGS := -std=c++14 -Wall -Wextra -O2 -Iinclude -I$(SYSTEMC_INC)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
```

Sources: [`Makefile:10-11`](Makefile#L10-L11), [`Makefile:29-33`](Makefile#L29-L33)

The executable target is named `project`, and it links SystemC plus the math library. [Source: `Makefile:8,26-27`](Makefile#L8-L8)

```make
$(TARGET): $(OBJECTS)
	$(CXX) -o $@ $^ $(LDFLAGS) $(LDLIBS)
```

Source: [`Makefile:26-27`](Makefile#L26-L27)

The public simulation function is wrapped in `extern "C"` so that C code can call a function implemented in C++ without C++ name mangling. [Source: `include/simulation.hpp:6-23`](include/simulation.hpp#L6-L23)

```cpp
#ifdef __cplusplus
extern "C" {
#endif

struct Result runSimulation(...);

#ifdef __cplusplus
}
#endif
```

Source: [`include/simulation.hpp:6-23`](include/simulation.hpp#L6-L23)

The real entry point is the C `main()`. A minimal `sc_main` symbol is also present because the installed SystemC library expects that symbol during linking. [Sources: `src/main.c:51-134`](src/main.c#L51-L134), [`src/simulation.cpp:6-10`](src/simulation.cpp#L6-L10)

## 3. Step one: command-line configuration

Program execution begins in `main()`. Default values are assigned before parsing, which guarantees a complete configuration even when the user supplies only the required CSV path. [Source: `src/main.c:51-58`](src/main.c#L51-L58)

```c
uint32_t cycles         = 1000;
uint32_t latencyRom     = 1;
uint32_t romSize        = 0;
uint32_t blockSize      = 0x1000;
const char* tracefile      = NULL;
const char* romContentPath = NULL;
```

Source: [`src/main.c:52-58`](src/main.c#L52-L58)

The supported options configure the simulation limit, optional trace, ROM latency, ROM size, protection-block size, and optional ROM content file. [Source: `src/main.c:60-69`](src/main.c#L60-L69)

`getopt_long()` processes each option, while `parse_uint_arg()` accepts decimal or hexadecimal text because `strtoul()` uses base zero. It also rejects an empty value, trailing garbage, conversion errors, and values larger than 32 bits. [Sources: `src/main.c:32-43`](src/main.c#L32-L43), [`src/main.c:71-83`](src/main.c#L71-L83)

```c
unsigned long v = strtoul(arg, &end, 0);
if (end == arg || *end != '\0' || errno != 0 || v > 0xFFFFFFFFUL) {
    /* report error and exit */
}
```

Source: [`src/main.c:33-42`](src/main.c#L33-L42)

The ROM size is restricted to zero or a power of two. The expression `x & (x - 1)` clears the least-significant set bit, so a result of zero means that at most one bit was set. The block size is independently checked against zero, and a positional CSV filename is required. [Sources: `src/main.c:45-49`](src/main.c#L45-L49), [`src/main.c:85-101`](src/main.c#L85-L101)

### Presentation command

A useful supervisor demonstration is:

```bash
./project --cycles 100 --latency-rom 2 --rom-size 16 \
  --block-size 16 --rom-content test/rom_content.txt \
  test/smoke.csv
```

This command is composed from the implemented options and the repository's ROM/smoke regression inputs. [Sources: `src/main.c:13-29`](src/main.c#L13-L29), [`test/rom_content.txt:1-2`](test/rom_content.txt#L1-L2), [`test/smoke.csv:1-5`](test/smoke.csv#L1-L5)

## 4. Step two: ROM file loading

The ROM file is an initialization image, not a file consulted on every read. `main()` calls the loader only when `--rom-content` is present and receives a dynamically allocated array of 32-bit words. [Source: `src/main.c:103-111`](src/main.c#L103-L111)

The loader receives a `uint32_t**` output parameter because it must change the pointer owned by `main()`. It initializes that output to `NULL`, calculates the word capacity as `romSizeBytes / 4`, opens the text file, and allocates zero-filled word storage with `calloc()`. [Source: `src/rom_loader.c:9-25`](src/rom_loader.c#L9-L25)

```c
*outRom = NULL;
uint32_t capacity = romSizeBytes / 4;
FILE* f = fopen(path, "r");
uint32_t* rom = calloc(capacity ? capacity : 1, sizeof(uint32_t));
```

Source: [`src/rom_loader.c:9-20`](src/rom_loader.c#L9-L20)

The ternary allocation uses at least one physical element even when logical capacity is zero, avoiding dependence on zero-byte allocation behavior. Logical capacity remains zero, so a non-empty ROM file is still rejected for a zero-sized ROM. `calloc()` also implements the required zero-fill behavior for ROM files containing fewer words than the configured ROM can hold. [Sources: `src/rom_loader.c:12-20`](src/rom_loader.c#L12-L20), [`include/rom_loader.h:10-16`](include/rom_loader.h#L10-L16)

The file is read line by line. Leading whitespace and blank lines are handled before checking capacity. The cast to `unsigned char` makes the `isspace()` call valid for every possible `char` value. [Source: `src/rom_loader.c:27-35`](src/rom_loader.c#L27-L35)

The parser rejects excess words before insertion, ensuring that the ROM file can never silently exceed the configured region. [Source: `src/rom_loader.c:37-44`](src/rom_loader.c#L37-L44)

Each non-empty line is parsed by `strtoul()` with base zero. The `end` pointer identifies unparsed characters, `errno` detects conversion range errors, and the explicit `0xFFFFFFFFUL` comparison enforces a 32-bit value even on platforms where `unsigned long` is wider. [Source: `src/rom_loader.c:46-56`](src/rom_loader.c#L46-L56)

```c
char* end = NULL;
errno = 0;
unsigned long val = strtoul(p, &end, 0);
while (*end && isspace((unsigned char)*end)) end++;
if (end == p || *end != '\0' || errno != 0 || val > 0xFFFFFFFFUL) {
    /* malformed or out-of-range ROM word */
}
```

Source: [`src/rom_loader.c:46-56`](src/rom_loader.c#L46-L56)

On success, `rom[count++] = (uint32_t)val` stores the word and increments the number of loaded words. At end-of-file, the loader closes the file, returns the allocated pointer through `*outRom`, and returns zero. [Source: `src/rom_loader.c:58-63`](src/rom_loader.c#L58-L63)

### ROM representation conversion

The loader returns words because every file line is a 32-bit number. The controller stores bytes because the memory address space must be byte-addressable. The controller constructor creates exactly `romSize` zero bytes, then splits each loaded word into four little-endian bytes. [Sources: `include/memory_controller.hpp:33-38`](include/memory_controller.hpp#L33-L38), [`src/memory_controller.cpp:3-24`](src/memory_controller.cpp#L3-L24)

```cpp
rom(romSize, 0)
```

Source: [`src/memory_controller.cpp:9-15`](src/memory_controller.cpp#L9-L15)

```cpp
rom[word * 4 + byte] = static_cast<uint8_t>(
    (value >> (byte * 8)) & 0xFF
);
```

Source: [`src/memory_controller.cpp:16-23`](src/memory_controller.cpp#L16-L23)

For the test word `0xAABBCCDD`, the byte vector stores `DD CC BB AA` at addresses zero through three. For `0x11223344`, it stores `44 33 22 11` at addresses four through seven. These example words are the actual ROM test data. [Sources: `test/rom_content.txt:1-2`](test/rom_content.txt#L1-L2), [`src/memory_controller.cpp:16-23`](src/memory_controller.cpp#L16-L23)

## 5. Step three: CSV request loading

The request format is represented by `struct Request`: address, data, write flag, user ID, and width flag. Read results later reuse the `data` field. `struct Result` contains the elapsed simulation cycles and number of access errors. [Source: `include/project_types.h:7-18`](include/project_types.h#L7-L18)

```c
struct Request {
  uint32_t addr;
  uint32_t data;
  uint8_t w;
  uint8_t user;
  uint8_t wide;
};
```

Source: [`include/project_types.h:7-13`](include/project_types.h#L7-L13)

`main()` asks `load_requests_csv()` to allocate and populate the array. If parsing fails, any previously allocated ROM array is freed before exiting. [Source: `src/main.c:113-119`](src/main.c#L113-L119)

The CSV parser uses two passes. The first pass counts non-empty lines, allowing one exact allocation. An empty file is accepted as zero requests. [Source: `src/csv_parser.c:30-62`](src/csv_parser.c#L30-L62)

The second pass rewinds the file, splits each line into exactly five comma-separated fields, trims whitespace in place, and converts the fields to a typed request. [Source: `src/csv_parser.c:72-109`](src/csv_parser.c#L72-L109)

The parser enforces these invariants:

- Operation is `R` or `W`. [Source: `src/csv_parser.c:111-119`](src/csv_parser.c#L111-L119)
- Address is a valid 32-bit decimal or hexadecimal number. [Sources: `src/csv_parser.c:19-28`](src/csv_parser.c#L19-L28), [`src/csv_parser.c:121-124`](src/csv_parser.c#L121-L124)
- A read has an empty data column; a write has a valid data value. [Source: `src/csv_parser.c:126-141`](src/csv_parser.c#L126-L141)
- User fits in `0..255`. [Source: `src/csv_parser.c:143-149`](src/csv_parser.c#L143-L149)
- Width is `T` for four bytes or `F` for one byte. [Source: `src/csv_parser.c:151-159`](src/csv_parser.c#L151-L159)
- A one-byte write value fits in eight bits. [Source: `src/csv_parser.c:161-165`](src/csv_parser.c#L161-L165)

The `FAIL` macro centralizes the parser's repeated cleanup path: report the specific error, free the partially built array, close the file, and return failure. [Source: `src/csv_parser.c:64-70`](src/csv_parser.c#L64-L70)

## 6. Step four: entering the SystemC simulation

After both inputs are prepared, `main()` calls `runSimulation()` with configuration, ROM words, request count, and request array. On return, it prints request count, cycles, and errors, then frees both dynamic arrays. [Source: `src/main.c:121-133`](src/main.c#L121-L133)

`runSimulation()` creates signals for the external memory-controller interface. In SystemC, these signals model wires carrying address, write data, operation flags, user, read data, readiness, and errors. [Source: `src/simulation.cpp:22-31`](src/simulation.cpp#L22-L31)

```cpp
sc_core::sc_signal<uint32_t> addrSignal;
sc_core::sc_signal<uint32_t> wdataSignal;
sc_core::sc_signal<bool> rSignal;
sc_core::sc_signal<bool> wSignal;
sc_core::sc_signal<bool> wideSignal;
sc_core::sc_signal<uint8_t> userSignal;
```

Source: [`src/simulation.cpp:22-27`](src/simulation.cpp#L22-L27)

The complete memory hierarchy is instantiated with the same configuration loaded by the C front end. [Source: `src/simulation.cpp:33-34`](src/simulation.cpp#L33-L34)

```cpp
MEMORY_SYSTEM memorySystem(
    "memorySystem", latencyRom, romSize, blockSize, romContent
);
```

Source: [`src/simulation.cpp:33-34`](src/simulation.cpp#L33-L34)

The signals are bound to the hierarchy's ports before simulation starts. [Source: `src/simulation.cpp:36-44`](src/simulation.cpp#L36-L44)

## 7. Step five: SystemC hierarchy and wiring

`MEMORY_SYSTEM` is a structural module. It contains the clock, internal controller-to-memory signals, one controller, and one main-memory module. It has no behavioral process of its own; its main role is ownership and wiring. [Source: `include/memory_system.hpp:10-32`](include/memory_system.hpp#L10-L32)

```text
MEMORY_SYSTEM
├── clk
├── externally visible request/result ports
├── internal memAddr/memWdata/memR/memW/memRdata/memReady wires
├── MEMORY_CONTROLLER controller
└── MAIN_MEMORY memory
```

The clock period is one nanosecond. The controller receives the configured ROM latency, ROM size, block size, and ROM words; main memory uses its default constructor. [Source: `include/memory_system.hpp:34-46`](include/memory_system.hpp#L34-L46)

`bindComponents()` connects external ports directly to the controller and connects the controller's internal memory interface to `MAIN_MEMORY` through hierarchy-owned signals. [Source: `include/memory_system.hpp:48-74`](include/memory_system.hpp#L48-L74)

Two readiness signals must be distinguished:

- External `ready` means the complete controller operation has finished. [Sources: `include/memory_system.hpp:18-20`](include/memory_system.hpp#L18-L20), [`include/memory_system.hpp:56-58`](include/memory_system.hpp#L56-L58)
- Internal `memReady` means the main-memory sub-operation has finished. [Sources: `include/memory_system.hpp:24-29`](include/memory_system.hpp#L24-L29), [`include/memory_system.hpp:60-73`](include/memory_system.hpp#L60-L73)

## 8. Step six: SystemC process registration and waiting

`MAIN_MEMORY` uses `SC_CTOR` for normal construction and retains an overload for explicitly configured latency. Both constructors call `registerProcess()`, which registers one clock-sensitive `SC_THREAD`. [Source: `include/main_memory.hpp:19-35`](include/main_memory.hpp#L19-L35)

```cpp
SC_CTOR(MAIN_MEMORY)
    : latency(1) {
    registerProcess();
}
```

Source: [`include/main_memory.hpp:22-25`](include/main_memory.hpp#L22-L25)

The controller uses `SC_HAS_PROCESS` because its constructor takes additional configuration parameters but still registers an `SC_THREAD`. [Sources: `include/memory_controller.hpp:40-48`](include/memory_controller.hpp#L40-L48), [`src/memory_controller.cpp:26-27`](src/memory_controller.cpp#L26-L27)

```cpp
SC_THREAD(behavior);
sensitive << clk.pos();
```

Source: [`src/memory_controller.cpp:26-27`](src/memory_controller.cpp#L26-L27)

An argumentless `wait()` inside either registered thread suspends that process until its next positive clock edge because of static clock sensitivity. A `wait(sc_core::SC_ZERO_TIME)` suspends only for a delta cycle, allowing pending `sc_signal` updates from another process at the same simulation time to become visible. The controller uses both waits while polling `mem_ready`. [Source: `src/memory_controller.cpp:237-250`](src/memory_controller.cpp#L237-L250)

This is a key expert point: SystemC signal writes are scheduled updates, not ordinary immediate variable assignments. The zero-time wait provides synchronization between the memory's signal writes and the controller's reads without advancing modeled time. The exact synchronization is visible after each clock wait in the RAM handshake. [Sources: `src/memory_controller.cpp:243-249`](src/memory_controller.cpp#L243-L249), [`src/memory_controller.cpp:259-264`](src/memory_controller.cpp#L259-L264)

## 9. Step seven: driving and accepting one request

Before processing requests, `runSimulation()` initializes all driver signals and performs a zero-time start so initialization processes and initial signal updates settle. [Source: `src/simulation.cpp:67-73`](src/simulation.cpp#L67-L73)

For each request, the driver writes its fields onto the SystemC signals. The CSV's single `w` flag is translated into mutually exclusive hardware read and write lines. [Source: `src/simulation.cpp:78-86`](src/simulation.cpp#L78-L86)

```cpp
addrSignal.write(requests[i].addr);
wdataSignal.write(requests[i].data);
userSignal.write(requests[i].user);
wideSignal.write(requests[i].wide != 0);
rSignal.write(requests[i].w == 0);
wSignal.write(requests[i].w != 0);
```

Source: [`src/simulation.cpp:81-86`](src/simulation.cpp#L81-L86)

The request remains asserted while simulation advances until the controller has had an opportunity to acknowledge it by lowering `ready`, reporting an immediate error, or reaching the driver's two-cycle safeguard. The driver then deasserts `r` and `w` and waits for completion. [Source: `src/simulation.cpp:88-103`](src/simulation.cpp#L88-L103)

At the controller's positive edge, `behavior()` snapshots both operation signals. If neither is asserted, it returns to waiting. Otherwise, it snapshots address, data, width, and user into local variables before the external driver deasserts the request. [Source: `src/memory_controller.cpp:40-54`](src/memory_controller.cpp#L40-L54)

The local variables are important because a transaction can take several cycles. Once accepted, it must use the original request values even after the external signals change. [Source: `src/memory_controller.cpp:43-54`](src/memory_controller.cpp#L43-L54)

## 10. Step eight: beginning and validating the controller transaction

`beginRequest()` lowers external `ready`, clears the previous error, and ensures no stale internal read/write command remains active. [Source: `src/memory_controller.cpp:111-116`](src/memory_controller.cpp#L111-L116)

```cpp
ready.write(false);
error.write(false);
mem_r.write(false);
mem_w.write(false);
```

Source: [`src/memory_controller.cpp:111-116`](src/memory_controller.cpp#L111-L116)

`requestIsValid()` performs three structural checks before any storage or ownership state changes. [Source: `src/memory_controller.cpp:118-136`](src/memory_controller.cpp#L118-L136)

First, `doRead == doWrite` rejects the invalid case where both operations are asserted. The no-operation case is already filtered earlier in `behavior()`. [Sources: `src/memory_controller.cpp:43-47`](src/memory_controller.cpp#L43-L47), [`src/memory_controller.cpp:123-126`](src/memory_controller.cpp#L123-L126)

Second, wide accesses are four bytes and narrow accesses are one byte. `rangeOverflows()` rejects a request whose final byte would wrap past `UINT32_MAX`. [Sources: `src/memory_controller.cpp:128-130`](src/memory_controller.cpp#L128-L130), [`src/memory_controller.cpp:182-184`](src/memory_controller.cpp#L182-L184)

Third, the controller checks whether the first and final accessed bytes both belong to the same mapped region. A wide request that starts in ROM and ends in RAM is rejected rather than partially reading two devices. [Source: `src/memory_controller.cpp:133-135`](src/memory_controller.cpp#L133-L135)

The regression `R,0x0f,,0,T` demonstrates this edge case for a 16-byte ROM: byte `0x0F` is ROM, but bytes `0x10..0x12` are RAM. [Sources: `test/rom_ram_boundary_error.csv:1`](test/rom_ram_boundary_error.csv#L1), [`Makefile:52`](Makefile#L52), [`src/memory_controller.cpp:128-135`](src/memory_controller.cpp#L128-L135)

## 11. Step nine: memory mapping and the ROM path

The mapping decision is intentionally simple: addresses smaller than `romSize` are ROM; all higher addresses are RAM. [Source: `src/memory_controller.cpp:186-188`](src/memory_controller.cpp#L186-L188)

```cpp
return currentAddr < romSize;
```

Source: [`src/memory_controller.cpp:186-188`](src/memory_controller.cpp#L186-L188)

If a request maps to ROM, a write immediately calls `finishError()`. A read calls `handleRomRead()` and never activates the main-memory interface. [Source: `src/memory_controller.cpp:61-69`](src/memory_controller.cpp#L61-L69)

`handleRomRead()` models configurable ROM latency by waiting for `latencyRom` positive edges. It then reconstructs the value, writes `rdata`, and completes successfully. [Source: `src/memory_controller.cpp:138-148`](src/memory_controller.cpp#L138-L148)

`romRead()` processes either one byte or four bytes and reconstructs a little-endian result by shifting each byte into position and OR-ing it into the accumulator. [Source: `src/memory_controller.cpp:190-200`](src/memory_controller.cpp#L190-L200)

```cpp
result |= static_cast<uint32_t>(value) << (i * 8);
```

Source: [`src/memory_controller.cpp:194-198`](src/memory_controller.cpp#L194-L198)

For the repository ROM image, a wide read at address zero reconstructs `0xAABBCCDD`, while a wide read at address four reconstructs `0x11223344`. Those two reads are the exact ROM regression input. [Sources: `test/rom_content.txt:1-2`](test/rom_content.txt#L1-L2), [`test/rom_read.csv:1-2`](test/rom_read.csv#L1-L2), [`src/memory_controller.cpp:190-200`](src/memory_controller.cpp#L190-L200)

`setRomAt()` is a special assignment-required helper that directly changes one byte if the address is in range. It does not contradict external read-only behavior because ordinary write requests still fail; this method is a privileged initialization/testing mechanism rather than a bus transaction. [Sources: `src/memory_controller.cpp:61-65`](src/memory_controller.cpp#L61-L65), [`src/memory_controller.cpp:105-109`](src/memory_controller.cpp#L105-L109)

## 12. Step ten: RAM protection and ownership

RAM accesses pass through `hasAccess()` before any main-memory command is issued. A denied access calls `finishError()` and never reaches `MAIN_MEMORY`. [Source: `src/memory_controller.cpp:71-74`](src/memory_controller.cpp#L71-L74)

Ownership is sparse: `owners` maps a protection-block index to an eight-bit user. Missing map entries represent unowned blocks. [Sources: `include/memory_controller.hpp:37-38`](include/memory_controller.hpp#L37-L38), [`src/memory_controller.cpp:90-102`](src/memory_controller.cpp#L90-L102)

The block index is relative to the end of ROM:

```cpp
return (currentAddr - romSize) / blockSize;
```

Source: [`src/memory_controller.cpp:203-205`](src/memory_controller.cpp#L203-L205)

For example, with `romSize = 0x10`, `blockSize = 0x10`, and `address = 0x100`, the block is `(0x100 - 0x10) / 0x10 = 15`. This formula is directly implemented by `blockForAddress()`. [Source: `src/memory_controller.cpp:203-205`](src/memory_controller.cpp#L203-L205)

Users zero and 255 always pass the access check. Every other user must either match the existing owner or access a block represented as free by owner value 255. [Source: `src/memory_controller.cpp:207-220`](src/memory_controller.cpp#L207-L220)

The controller checks every byte of a wide request rather than only its starting address. This is essential when four bytes cross a protection-block boundary. [Source: `src/memory_controller.cpp:212-218`](src/memory_controller.cpp#L212-L218)

Writes update every touched block. A normal user or user zero becomes the owner; user 255 erases ownership and therefore releases the block. [Source: `src/memory_controller.cpp:223-235`](src/memory_controller.cpp#L223-L235)

User 255 also releases ownership on a read. That behavior is explicitly applied before the RAM read. [Source: `src/memory_controller.cpp:76-80`](src/memory_controller.cpp#L76-L80)

The `user255_release_read.csv` sequence proves this policy: user 7 first claims the block, user 255 reads and releases it, and user 8 can then write without an error. [Sources: `test/user255_release_read.csv:1-3`](test/user255_release_read.csv#L1-L3), [`Makefile:51`](Makefile#L51), [`src/memory_controller.cpp:76-80`](src/memory_controller.cpp#L76-L80), [`src/memory_controller.cpp:223-235`](src/memory_controller.cpp#L223-L235)

## 13. Step eleven: four-byte and one-byte RAM reads

`handleMemoryRead()` always requests a 32-bit value from main memory. For a wide access it returns the full value; for a narrow access it masks the least-significant byte and thereby zeroes the other 24 output bits. [Source: `src/memory_controller.cpp:150-156`](src/memory_controller.cpp#L150-L156)

```cpp
uint32_t value = memoryRead(currentAddr);
rdata.write(currentWide ? value : (value & 0xFF));
```

Source: [`src/memory_controller.cpp:150-156`](src/memory_controller.cpp#L150-L156)

This works for unaligned addresses because `MAIN_MEMORY::get()` begins its four-byte assembly at the exact requested byte address. The requested byte therefore occupies bits `7..0`, which is exactly what the controller keeps. [Sources: `include/main_memory.hpp:69-84`](include/main_memory.hpp#L69-L84), [`src/memory_controller.cpp:150-156`](src/memory_controller.cpp#L150-L156)

## 14. Step twelve: four-byte and one-byte RAM writes

A wide write updates ownership and forwards the complete 32-bit value directly to `memoryWrite()`. [Source: `src/memory_controller.cpp:158-169`](src/memory_controller.cpp#L158-L169)

A one-byte write must preserve the other three bytes because main memory only accepts four-byte writes. The controller therefore performs read-modify-write: read four existing bytes, clear only the least-significant byte, insert the new eight-bit value, and write the reconstructed word back to the same byte address. [Source: `src/memory_controller.cpp:171-174`](src/memory_controller.cpp#L171-L174)

```cpp
uint32_t oldValue = memoryRead(currentAddr);
uint32_t newValue =
    (oldValue & 0xFFFFFF00u) | (currentData & 0xFFu);
memoryWrite(currentAddr, newValue);
```

Source: [`src/memory_controller.cpp:171-174`](src/memory_controller.cpp#L171-L174)

The `byte_unaligned.csv` regression first writes `0x11223344` at `0x100`, then writes byte `0xAA` at `0x101`, then reads the byte and the surrounding wide word. Given little-endian storage, the final wide value at `0x100` should be `0x1122AA44`. [Sources: `test/byte_unaligned.csv:1-4`](test/byte_unaligned.csv#L1-L4), [`include/main_memory.hpp:86-94`](include/main_memory.hpp#L86-L94), [`src/memory_controller.cpp:171-174`](src/memory_controller.cpp#L171-L174)

## 15. Step thirteen: controller-to-memory handshake

For an internal read, the controller writes the address, clears write data, deasserts `mem_w`, and asserts `mem_r`. It then waits one clock edge and one delta cycle repeatedly until `mem_ready` becomes true. Finally, it samples `mem_rdata` and deasserts `mem_r`. [Source: `src/memory_controller.cpp:237-250`](src/memory_controller.cpp#L237-L250)

For an internal write, it writes address and data, asserts only `mem_w`, waits by the same protocol, and then deasserts `mem_w`. [Source: `src/memory_controller.cpp:253-265`](src/memory_controller.cpp#L253-L265)

This handshake decouples controller behavior from main-memory latency. The controller does not assume completion after a fixed number of its own cycles; it observes the subordinate module's explicit readiness. [Sources: `src/memory_controller.cpp:243-246`](src/memory_controller.cpp#L243-L246), [`src/memory_controller.cpp:259-262`](src/memory_controller.cpp#L259-L262)

## 16. Step fourteen: main-memory operation

`MAIN_MEMORY` stores bytes in `std::map<uint32_t, uint8_t>`. A sparse map avoids allocating a full 32-bit address space; unwritten addresses are interpreted as zero. [Sources: `include/main_memory.hpp:19-20`](include/main_memory.hpp#L19-L20), [`include/main_memory.hpp:69-83`](include/main_memory.hpp#L69-L83)

The memory thread initializes `ready` and `rdata`, waits for positive edges, ignores idle cycles, snapshots the request, lowers `ready`, waits its configured latency, performs the operation, and raises `ready`. [Source: `include/main_memory.hpp:37-67`](include/main_memory.hpp#L37-L67)

`set()` stores a 32-bit value as four little-endian bytes. For `0x11223344` at address `0x100`, the map stores `0x44`, `0x33`, `0x22`, and `0x11` at `0x100..0x103`. [Source: `include/main_memory.hpp:86-94`](include/main_memory.hpp#L86-L94)

`get()` reverses that transformation: it looks up each byte, substitutes zero for absent addresses, shifts by `i * 8`, and combines the bytes. [Source: `include/main_memory.hpp:69-84`](include/main_memory.hpp#L69-L84)

Both functions guard the final `UINT32_MAX` byte so the loop does not wrap the address to zero. [Sources: `include/main_memory.hpp:78-80`](include/main_memory.hpp#L78-L80), [`include/main_memory.hpp:91-93`](include/main_memory.hpp#L91-L93)

## 17. Step fifteen: success, error, and result collection

A successful controller request clears `error` and raises `ready`. [Source: `src/memory_controller.cpp:177-180`](src/memory_controller.cpp#L177-L180)

An error cancels both internal commands, raises `error`, and also raises `ready`, meaning that the failed request has completed and the requester may continue. [Source: `src/memory_controller.cpp:267-272`](src/memory_controller.cpp#L267-L272)

Back in `runSimulation()`, the driver counts an asserted error. On a successful read, it copies `rdataSignal` into the current `Request.data`, so the request array contains its own read results after simulation. [Source: `src/simulation.cpp:105-113`](src/simulation.cpp#L105-L113)

The function returns elapsed cycles and total errors through `struct Result`. [Sources: `src/simulation.cpp:120-123`](src/simulation.cpp#L120-L123), [`include/project_types.h:15-18`](include/project_types.h#L15-L18)

The optional VCD trace is closed before returning, and `main()` frees both ROM and request arrays after it prints the result. [Sources: `src/simulation.cpp:116-118`](src/simulation.cpp#L116-L118), [`src/main.c:125-133`](src/main.c#L125-L133)

## 18. Tracing and demonstrating timing

When `--tf` is set, `runSimulation()` creates a VCD trace and records both external and internal signals: clock, request interface, result interface, and controller-to-memory interface. [Source: `src/simulation.cpp:46-65`](src/simulation.cpp#L46-L65)

For a RAM read, the useful waveform story is:

1. External `r` is asserted with `addr`, `wide`, and `user`. [Source: `src/simulation.cpp:81-86`](src/simulation.cpp#L81-L86)
2. At a positive edge, the controller snapshots the request and lowers external `ready`. [Sources: `src/memory_controller.cpp:40-54`](src/memory_controller.cpp#L40-L54), [`src/memory_controller.cpp:111-116`](src/memory_controller.cpp#L111-L116)
3. After validation and permission checks, internal `mem_r` is asserted. [Sources: `src/memory_controller.cpp:56-80`](src/memory_controller.cpp#L56-L80), [`src/memory_controller.cpp:237-242`](src/memory_controller.cpp#L237-L242)
4. Main memory lowers internal readiness, waits its latency, writes `mem_rdata`, and raises internal readiness. [Source: `include/main_memory.hpp:50-65`](include/main_memory.hpp#L50-L65)
5. The controller observes `mem_ready`, captures the value, deasserts `mem_r`, writes external `rdata`, and raises external `ready`. [Sources: `src/memory_controller.cpp:243-250`](src/memory_controller.cpp#L243-L250), [`src/memory_controller.cpp:150-156`](src/memory_controller.cpp#L150-L156), [`src/memory_controller.cpp:177-180`](src/memory_controller.cpp#L177-L180)

## 19. Fully worked examples

### Example A: ROM read

Configuration uses a 16-byte ROM initialized with `0xAABBCCDD` and `0x11223344`. The request file reads addresses zero and four as wide values. [Sources: `Makefile:46`](Makefile#L46), [`test/rom_content.txt:1-2`](test/rom_content.txt#L1-L2), [`test/rom_read.csv:1-2`](test/rom_read.csv#L1-L2)

The loader creates four zero-filled word slots and replaces the first two from the file. The controller converts them into eight initialized little-endian bytes plus eight zero bytes. [Sources: `src/rom_loader.c:12-20`](src/rom_loader.c#L12-L20), [`src/rom_loader.c:58-63`](src/rom_loader.c#L58-L63), [`src/memory_controller.cpp:14-23`](src/memory_controller.cpp#L14-L23)

At address zero, mapping selects ROM, the controller waits `latencyRom`, and `romRead()` reconstructs `0xAABBCCDD`. Address four similarly produces `0x11223344`. [Sources: `src/memory_controller.cpp:61-68`](src/memory_controller.cpp#L61-L68), [`src/memory_controller.cpp:138-148`](src/memory_controller.cpp#L138-L148), [`src/memory_controller.cpp:190-200`](src/memory_controller.cpp#L190-L200)

### Example B: ROM write error

The request `W,0x0,0x12345678,0,T` targets address zero in the configured ROM region. The controller detects `doWrite` after mapping selects ROM and completes with `error = true` without activating main memory. [Sources: `test/rom_write_error.csv:1`](test/rom_write_error.csv#L1), [`Makefile:47`](Makefile#L47), [`src/memory_controller.cpp:61-65`](src/memory_controller.cpp#L61-L65), [`src/memory_controller.cpp:267-272`](src/memory_controller.cpp#L267-L272)

### Example C: wide RAM write and read

The test writes `0x11223344` at `0x100` as user 1 and then reads it as the same user. [Source: `test/ram_wide.csv:1-2`](test/ram_wide.csv#L1-L2)

Address `0x100` is above the 16-byte ROM, so mapping selects RAM. The block is initially free, the write claims it for user 1, `memoryWrite()` waits for internal completion, and main memory stores four little-endian bytes. The following user-1 read passes ownership and reconstructs the same 32-bit value. [Sources: `src/memory_controller.cpp:71-86`](src/memory_controller.cpp#L71-L86), [`src/memory_controller.cpp:207-235`](src/memory_controller.cpp#L207-L235), [`src/memory_controller.cpp:253-265`](src/memory_controller.cpp#L253-L265), [`include/main_memory.hpp:69-94`](include/main_memory.hpp#L69-L94)

### Example D: unaligned byte write

The regression writes `0x11223344` at `0x100`, then writes byte `0xAA` at `0x101`, reads that byte, and finally reads the wide word at `0x100`. [Source: `test/byte_unaligned.csv:1-4`](test/byte_unaligned.csv#L1-L4)

Initially, bytes `0x100..0x103` are `44 33 22 11`. A four-byte read beginning at `0x101` returns bytes `33 22 11 00`, represented as `0x00112233`. Read-modify-write produces `0x001122AA` and writes it from `0x101`, preserving address `0x100`. The final bytes from `0x100` are `44 AA 22 11`, which reconstruct as `0x1122AA44`. The byte ordering comes from `MAIN_MEMORY::get()`/`set()`, and the masking comes from `handleMemoryWrite()`. [Sources: `include/main_memory.hpp:69-94`](include/main_memory.hpp#L69-L94), [`src/memory_controller.cpp:158-174`](src/memory_controller.cpp#L158-L174)

### Example E: permission denial and release

The permission test has user 7 claim a block, user 8 attempt a denied read, user 7 perform an allowed read, user 255 release the block through a write, and user 8 then claim it. [Source: `test/permissions.csv:1-5`](test/permissions.csv#L1-L5)

This sequence exercises normal ownership, rejection, owner access, special user-255 release, and reassignment. [Sources: `src/memory_controller.cpp:207-235`](src/memory_controller.cpp#L207-L235), [`Makefile:54`](Makefile#L54)

### Example F: block-boundary permissions

The boundary regression gives adjacent blocks to users 1 and 2, then performs a wide read beginning at `0x1F`. That read touches both blocks and is rejected for user 1 because permission is checked for every byte. User zero repeats the same cross-block access successfully because it is privileged. [Sources: `test/block_boundary_permissions.csv:1-4`](test/block_boundary_permissions.csv#L1-L4), [`src/memory_controller.cpp:207-220`](src/memory_controller.cpp#L207-L220), [`Makefile:50`](Makefile#L50)

## 20. Smart design decisions to highlight

### Separate software preparation from hardware behavior

The C files handle text, allocation, and CLI validation, while C++ files model clocked hardware. This keeps file parsing out of the SystemC module and gives `runSimulation()` a clear boundary. [Sources: `Makefile:15-20`](Makefile#L15-L20), [`src/main.c:103-124`](src/main.c#L103-L124), [`src/simulation.cpp:12-21`](src/simulation.cpp#L12-L21)

### Use a structural hierarchy instead of wiring everything in the driver

`MEMORY_SYSTEM` owns the controller, memory, clock, and internal bus. `runSimulation()` sees one subsystem rather than managing internal device wiring. This resembles a hardware top-level module and makes later integration clearer. [Sources: `include/memory_system.hpp:22-46`](include/memory_system.hpp#L22-L46), [`include/memory_system.hpp:48-74`](include/memory_system.hpp#L48-L74)

### Store ROM and RAM as bytes internally

The external interface supports one-byte and four-byte operations at arbitrary addresses. Byte storage makes little-endian assembly explicit and supports unaligned requests without allocating a full dense memory. [Sources: `include/memory_controller.hpp:37`](include/memory_controller.hpp#L37), [`include/main_memory.hpp:19`](include/main_memory.hpp#L19), [`src/memory_controller.cpp:190-200`](src/memory_controller.cpp#L190-L200), [`include/main_memory.hpp:69-94`](include/main_memory.hpp#L69-L94)

### Keep the main-memory interface uniformly four bytes

Width adaptation belongs in the controller. A narrow read masks the full memory result, and a narrow write uses read-modify-write, leaving `MAIN_MEMORY` with one consistent 32-bit transaction interface. [Sources: `src/memory_controller.cpp:150-174`](src/memory_controller.cpp#L150-L174), [`include/main_memory.hpp:11-17`](include/main_memory.hpp#L11-L17)

### Check permission for every accessed byte

This avoids the security error of authorizing only the first byte of a cross-block access. [Source: `src/memory_controller.cpp:212-218`](src/memory_controller.cpp#L212-L218)

### Reject cross-device operations atomically

A wide request crossing the ROM/RAM boundary is rejected as one transaction instead of partially returning ROM and RAM bytes. This makes device selection deterministic and avoids partial side effects. [Source: `src/memory_controller.cpp:128-135`](src/memory_controller.cpp#L128-L135)

### Copy ROM data into controller-owned storage

The controller does not retain a dependency on the temporary C allocation. After construction, the C array can safely be freed after simulation because the controller owns a byte-vector copy. [Sources: `src/memory_controller.cpp:14-24`](src/memory_controller.cpp#L14-L24), [`src/main.c:121-132`](src/main.c#L121-L132)

### Use sparse maps for large conceptual address spaces

Both RAM bytes and block owners only consume storage when used. This is appropriate for simulation, where test requests touch very few of the possible 32-bit addresses. [Sources: `include/main_memory.hpp:19`](include/main_memory.hpp#L19), [`include/memory_controller.hpp:38`](include/memory_controller.hpp#L38)

## 21. Edge-case checklist

| Edge case | Implemented behavior | Exact source |
|---|---|---|
| Neither `r` nor `w` asserted | Ignore cycle and wait again | [`src/memory_controller.cpp:43-47`](src/memory_controller.cpp#L43-L47) |
| Both `r` and `w` asserted | Reject request | [`src/memory_controller.cpp:118-126`](src/memory_controller.cpp#L118-L126) |
| 32-bit address-range overflow | Reject before adding final byte | [`src/memory_controller.cpp:128-130`](src/memory_controller.cpp#L128-L130), [`src/memory_controller.cpp:182-184`](src/memory_controller.cpp#L182-L184) |
| Wide access crosses ROM/RAM | Reject atomically | [`src/memory_controller.cpp:133-135`](src/memory_controller.cpp#L133-L135) |
| Write targets ROM | Immediate access error | [`src/memory_controller.cpp:61-65`](src/memory_controller.cpp#L61-L65) |
| ROM file shorter than ROM | Remaining words/bytes stay zero | [`src/rom_loader.c:20,58-63`](src/rom_loader.c#L20), [`src/memory_controller.cpp:14-23`](src/memory_controller.cpp#L14-L23) |
| ROM file longer than ROM | Loader error and cleanup | [`src/rom_loader.c:37-44`](src/rom_loader.c#L37-L44) |
| Malformed ROM value | Reject with physical line number | [`src/rom_loader.c:46-56`](src/rom_loader.c#L46-L56) |
| Invalid CSV columns/type/data/user/width | Parser error and cleanup | [`src/csv_parser.c:83-165`](src/csv_parser.c#L83-L165) |
| One-byte write data exceeds `0xFF` | Parser rejects it | [`src/csv_parser.c:161-165`](src/csv_parser.c#L161-L165) |
| Read from unwritten RAM | Missing bytes become zero | [`include/main_memory.hpp:69-76`](include/main_memory.hpp#L69-L76) |
| Wide access crosses protection blocks | Check each touched byte/block | [`src/memory_controller.cpp:212-218`](src/memory_controller.cpp#L212-L218) |
| User zero accesses owned block | Always allowed | [`src/memory_controller.cpp:207-210`](src/memory_controller.cpp#L207-L210) |
| User 255 accesses owned block | Always allowed and ownership erased | [`src/memory_controller.cpp:207-210`](src/memory_controller.cpp#L207-L210), [`src/memory_controller.cpp:223-235`](src/memory_controller.cpp#L223-L235) |
| Main memory has latency | Controller waits on `mem_ready` | [`src/memory_controller.cpp:237-265`](src/memory_controller.cpp#L237-L265) |
| Cycle budget expires | Request loop stops at configured limit | [`src/simulation.cpp:78-106`](src/simulation.cpp#L78-L106) |

## 22. Honest limitations and next improvements

Being able to explain limitations makes the presentation stronger because it shows engineering judgment rather than claiming perfection.

### Tests verify error counts, not read data

The Makefile captures program output and checks only the `Errors:` line. It does not assert ROM values, RAM values, byte masks, or read-modify-write results, even though `runSimulation()` stores successful reads back into `Request.data`. [Sources: `Makefile:35-54`](Makefile#L35-L54), [`src/simulation.cpp:109-113`](src/simulation.cpp#L109-L113)

**Improvement:** add a C++ test harness that calls `runSimulation()` and checks exact post-simulation `Request.data` values, such as `0xAABBCCDD`, `0x11223344`, and `0x1122AA44`. Those expected values derive from the repository ROM and byte-unaligned scenarios. [Sources: `test/rom_content.txt:1-2`](test/rom_content.txt#L1-L2), [`test/byte_unaligned.csv:1-4`](test/byte_unaligned.csv#L1-L4)

### Request acceptance uses a two-cycle safeguard

The driver waits for `ready` to fall, an error, or two elapsed cycles before deasserting the request. This prevents a request from being removed before the controller sees it, but it is less explicit than a defined one-cycle request pulse and can be ambiguous for zero-latency operations. [Source: `src/simulation.cpp:78-103`](src/simulation.cpp#L78-L103)

**Improvement:** assert each request for exactly one known clock edge, deassert it after acceptance, and then wait only for completion. Alternatively, add an explicit accepted/busy state inside the testbench if the interface may be extended.

### Header changes do not create dependency files

The Makefile's object rules depend only on source files, so an incremental `make` may not rebuild a `.cpp` file after only an included header changes. [Source: `Makefile:29-33`](Makefile#L29-L33)

**Improvement:** add `-MMD -MP`, include generated `.d` files, and remove them in `clean`.

### Duplicate completion text

`main()` currently prints `Simulation finished.` twice. [Source: `src/main.c:125-126`](src/main.c#L125-L126)

**Improvement:** remove one print before the presentation.

### Extra positional arguments are not rejected

The code checks that at least one CSV path exists and uses `argv[optind]`, but does not reject additional positional arguments. [Source: `src/main.c:96-101`](src/main.c#L96-L101)

**Improvement:** verify `optind + 1 == argc` after selecting the input file.

### Incomplete request processing is not reported separately

When the cycle budget expires, the loop stops, but `Result` contains only cycles and errors, so the caller cannot directly distinguish complete processing from a timeout. [Sources: `src/simulation.cpp:78-106`](src/simulation.cpp#L78-L106), [`include/project_types.h:15-18`](include/project_types.h#L15-L18)

**Improvement:** print a warning or, if allowed to extend the result, add processed-request or timeout information.

### Internal methods are publicly visible

Because `SC_MODULE` expands to a struct and no `private:` section is introduced, helper functions and internal state are public along with the required `getOwner()` and `setRomAt()` methods. [Source: `include/memory_controller.hpp:9-79`](include/memory_controller.hpp#L9-L79)

**Improvement:** keep the required methods public and move validation, mapping, ownership, and RAM handshake helpers into a private section.

### Clock ownership limits future subsystem reuse

`MEMORY_SYSTEM` owns its own clock, which is convenient for this standalone simulator. A future TinyRISC integration would more naturally receive an external CPU clock so all modules share the same timing domain. [Sources: `include/memory_system.hpp:22`](include/memory_system.hpp#L22), [`include/memory_system.hpp:41-44`](include/memory_system.hpp#L41-L44)

**Improvement:** if integrating into a CPU, replace the owned `sc_clock` with an `sc_in<bool> clk` and bind a top-level CPU/testbench clock.

### Parser line buffers are fixed-size

The CSV and ROM loaders use 512-byte and 256-byte line buffers. Valid assignment inputs are much shorter, but an extremely long malformed line can be read in fragments and produce a confusing error. [Sources: `src/csv_parser.c:43-49`](src/csv_parser.c#L43-L49), [`src/rom_loader.c:27-30`](src/rom_loader.c#L27-L30)

**Improvement:** detect a missing newline when a buffer fills or use a dynamically sized line reader where permitted.

## 23. Questions to ask the supervisor

These questions demonstrate that you understand the current implementation and are thinking about architecture, verification, and integration.

### Interface and timing questions

1. **Would you prefer the request input to be specified as a one-cycle pulse, or should the requester hold `r`/`w` until the controller visibly accepts the request?** The current driver holds the request until readiness changes or its safeguard triggers. [Source: `src/simulation.cpp:78-103`](src/simulation.cpp#L78-L103)

2. **For `latencyRom = 0`, should completion be visible in the same delta cycle, or should every request require at least one clock edge?** ROM latency is implemented as exactly `latencyRom` argumentless waits. [Source: `src/memory_controller.cpp:138-148`](src/memory_controller.cpp#L138-L148)

3. **Would a reusable memory subsystem be better with an external clock port instead of owning its clock?** The current hierarchy owns a one-nanosecond `sc_clock`. [Source: `include/memory_system.hpp:22,34-45`](include/memory_system.hpp#L22)

4. **Should `ready` represent idle state, a one-cycle completion pulse, or both?** The current controller initializes it high, lowers it while busy, and raises it on both success and error. [Sources: `src/memory_controller.cpp:31-38`](src/memory_controller.cpp#L31-L38), [`src/memory_controller.cpp:111-116`](src/memory_controller.cpp#L111-L116), [`src/memory_controller.cpp:177-180`](src/memory_controller.cpp#L177-L180), [`src/memory_controller.cpp:267-272`](src/memory_controller.cpp#L267-L272)

### Memory semantics questions

5. **Should a four-byte request crossing the ROM/RAM boundary always fail atomically, or is a split transaction ever desirable?** The current implementation rejects it before any side effect. [Source: `src/memory_controller.cpp:128-135`](src/memory_controller.cpp#L128-L135)

6. **Should the main-memory interface permit arbitrary unaligned four-byte addresses, as it currently does, or should the controller align accesses and shift bytes?** `MAIN_MEMORY::get()` and `set()` start at the exact supplied byte address. [Source: `include/main_memory.hpp:69-94`](include/main_memory.hpp#L69-L94)

7. **Would it be clearer to use a named owner-state structure instead of overloading value 255 as both the privileged release user and the free-owner sentinel?** The current logic uses 255 in both roles. [Sources: `src/memory_controller.cpp:90-102`](src/memory_controller.cpp#L90-L102), [`src/memory_controller.cpp:207-235`](src/memory_controller.cpp#L207-L235)

8. **Should ownership be assigned before the RAM write starts, as it is now, or only after successful memory completion?** Ownership is updated before `memoryWrite()` or read-modify-write. [Source: `src/memory_controller.cpp:158-174`](src/memory_controller.cpp#L158-L174)

### Verification questions

9. **Would you recommend direct SystemC unit tests for `getOwner()` and `setRomAt()` in addition to end-to-end CSV tests?** Those methods are part of the controller's public interface. [Sources: `include/memory_controller.hpp:50-53`](include/memory_controller.hpp#L50-L53), [`src/memory_controller.cpp:90-109`](src/memory_controller.cpp#L90-L109)

10. **Should the test framework verify exact read data and processed-request count, rather than only errors?** The current Makefile only greps error counts, although read values are stored into requests. [Sources: `Makefile:35-54`](Makefile#L35-L54), [`src/simulation.cpp:109-113`](src/simulation.cpp#L109-L113)

11. **What additional timing assertions would you expect in the VCD trace—for example, that `mem_r` and `mem_w` are never high together and that external `ready` remains low until internal completion?** All relevant signals are traced. [Source: `src/simulation.cpp:46-65`](src/simulation.cpp#L46-L65)

12. **Should reaching the cycle limit be treated as an error, timeout, or normal partial result?** The current loop simply stops when the configured limit is reached. [Source: `src/simulation.cpp:78-106`](src/simulation.cpp#L78-L106)

### Software-quality questions

13. **Would generated header dependencies with `-MMD -MP` be expected for reliable incremental builds?** Current object rules mention only source files. [Source: `Makefile:29-33`](Makefile#L29-L33)

14. **Would you prefer the controller implemented as the current blocking `SC_THREAD`, or as an explicit finite-state machine for clearer synthesis-oriented timing?** The current process delegates phases to helper functions that may wait. [Sources: `src/memory_controller.cpp:26-88`](src/memory_controller.cpp#L26-L88), [`src/memory_controller.cpp:138-174`](src/memory_controller.cpp#L138-L174)

15. **Should sparse `std::map` storage remain for simulation clarity, or would a dense vector/array better represent the intended hardware size?** RAM and ownership are both sparse maps. [Sources: `include/main_memory.hpp:19`](include/main_memory.hpp#L19), [`include/memory_controller.hpp:38`](include/memory_controller.hpp#L38)

## 24. Questions the supervisor may ask you

### Why is ROM loaded as words but stored as bytes?

The file format provides one 32-bit value per line, so the C loader naturally returns words. The memory address space supports byte addressing, so the controller converts each word into four little-endian bytes. [Sources: `include/rom_loader.h:10-16`](include/rom_loader.h#L10-L16), [`src/memory_controller.cpp:14-23`](src/memory_controller.cpp#L14-L23)

### Why does a byte write need a read first?

Main memory accepts only four-byte writes. Reading the existing four bytes and replacing only the least-significant byte prevents the other three bytes from being destroyed. [Source: `src/memory_controller.cpp:158-174`](src/memory_controller.cpp#L158-L174)

### Why is `wait(SC_ZERO_TIME)` needed?

After a clock edge, another process may schedule a signal update for the same simulation time. The zero-time wait allows that update phase to complete before the controller samples `mem_ready` or `mem_rdata`. [Source: `src/memory_controller.cpp:243-249`](src/memory_controller.cpp#L243-L249)

### Why check every byte's owner?

A four-byte request can cross a protection-block boundary. Checking only the starting address would allow unauthorized bytes in the following block. [Source: `src/memory_controller.cpp:212-218`](src/memory_controller.cpp#L212-L218)

### Why does user 255 erase ownership?

The special release semantics are implemented by erasing every touched block from the sparse ownership map. An absent entry then returns the free sentinel 255. [Sources: `src/memory_controller.cpp:90-102`](src/memory_controller.cpp#L90-L102), [`src/memory_controller.cpp:223-235`](src/memory_controller.cpp#L223-L235)

### Why are there two `ready` signals?

External `ready` describes completion of the complete mapped/protected/adapted controller request. Internal `memReady` describes only the subordinate main-memory operation. ROM requests never need internal memory readiness. [Sources: `include/memory_system.hpp:18-29`](include/memory_system.hpp#L18-L29), [`src/memory_controller.cpp:61-68`](src/memory_controller.cpp#L61-L68), [`src/memory_controller.cpp:237-265`](src/memory_controller.cpp#L237-L265)

### Why use a sparse map?

The conceptual address space is 32 bits, but tests touch few addresses. A sparse map allocates storage only for written bytes and treats absent bytes as zero. [Sources: `include/main_memory.hpp:19`](include/main_memory.hpp#L19), [`include/main_memory.hpp:69-76`](include/main_memory.hpp#L69-L76)

### Why use `SC_HAS_PROCESS` instead of `SC_CTOR` for the controller?

The controller requires configuration parameters in addition to its module name. `SC_HAS_PROCESS` supports its custom constructor while allowing it to register `SC_THREAD(behavior)`. [Sources: `include/memory_controller.hpp:40-48`](include/memory_controller.hpp#L40-L48), [`src/memory_controller.cpp:3-28`](src/memory_controller.cpp#L3-L28)

## 25. Presentation navigation map

Use this map when your supervisor asks to see a specific part:

| Topic | Code location |
|---|---|
| CLI help and configuration | [`src/main.c:13-101`](src/main.c#L13-L101) |
| ROM allocation and parsing | [`src/rom_loader.c:9-63`](src/rom_loader.c#L9-L63) |
| CSV parsing and validation | [`src/csv_parser.c:30-175`](src/csv_parser.c#L30-L175) |
| Request and Result definitions | [`include/project_types.h:7-18`](include/project_types.h#L7-L18) |
| C/C++ simulation interface | [`include/simulation.hpp:6-23`](include/simulation.hpp#L6-L23) |
| Simulation driver and results | [`src/simulation.cpp:12-123`](src/simulation.cpp#L12-L123) |
| Trace creation | [`src/simulation.cpp:46-65`](src/simulation.cpp#L46-L65) |
| Memory-system hierarchy | [`include/memory_system.hpp:10-74`](include/memory_system.hpp#L10-L74) |
| Controller ports/configuration | [`include/memory_controller.hpp:9-48`](include/memory_controller.hpp#L9-L48) |
| Controller transaction flow | [`src/memory_controller.cpp:31-88`](src/memory_controller.cpp#L31-L88) |
| ROM initialization | [`src/memory_controller.cpp:3-24`](src/memory_controller.cpp#L3-L24) |
| Request validation | [`src/memory_controller.cpp:118-136`](src/memory_controller.cpp#L118-L136) |
| ROM latency and reads | [`src/memory_controller.cpp:138-148`](src/memory_controller.cpp#L138-L148), [`src/memory_controller.cpp:186-200`](src/memory_controller.cpp#L186-L200) |
| Permission checking | [`src/memory_controller.cpp:90-102`](src/memory_controller.cpp#L90-L102), [`src/memory_controller.cpp:203-235`](src/memory_controller.cpp#L203-L235) |
| Width adaptation | [`src/memory_controller.cpp:150-174`](src/memory_controller.cpp#L150-L174) |
| RAM handshake | [`src/memory_controller.cpp:237-265`](src/memory_controller.cpp#L237-L265) |
| Completion and errors | [`src/memory_controller.cpp:177-180`](src/memory_controller.cpp#L177-L180), [`src/memory_controller.cpp:267-272`](src/memory_controller.cpp#L267-L272) |
| Main-memory process | [`include/main_memory.hpp:37-67`](include/main_memory.hpp#L37-L67) |
| Little-endian RAM storage | [`include/main_memory.hpp:69-94`](include/main_memory.hpp#L69-L94) |
| Regression suite | [`Makefile:35-54`](Makefile#L35-L54) |

## 26. Final presentation close

> The central design choice was to make the memory controller the policy and adaptation layer. The C front end only prepares validated inputs. The top-level SystemC hierarchy cleanly separates controller and storage. The controller performs atomic validation before side effects, maps ROM and RAM by address, models latency with clocked waits, checks ownership for every byte, and adapts between one-byte client operations and a uniform four-byte memory interface. Main memory is deliberately simple: sparse byte storage plus a ready handshake. The design already covers ROM protection, unaligned byte operations, cross-block permissions, special users, overflow, ROM/RAM boundaries, and tracing. The next engineering step is stronger value-based verification and a more explicit request-acceptance protocol.

Every claim in this summary maps to the controller dispatch, ownership logic, width-adaptation helpers, RAM handshake, storage implementation, and current regression suite. [Sources: `src/memory_controller.cpp:31-88`](src/memory_controller.cpp#L31-L88), [`src/memory_controller.cpp:118-272`](src/memory_controller.cpp#L118-L272), [`include/main_memory.hpp:37-94`](include/main_memory.hpp#L37-L94), [`Makefile:35-54`](Makefile#L35-L54)
