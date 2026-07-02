#include <systemc.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>

#include "tinyRISC.hpp"

int sc_main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  std::map<uint32_t, uint32_t> initialMemory;
  const uint32_t program[] = {
    0x00006533,
    0x00A545B3,
    0x00B5C633,
    0x00C646B3,
    0x00D6C733,
    0x00E747B3,
    0x00F7C833,
    0x010848B3,
    0x0118C933,
    0x012949B3,
    0x0139CA33,
    0x014A4AB3,
    0x015ACB33,
    0x010B42B3,
    0x00D5C0B3,
    0x000061B3,
    0x00124233,
    0x0030D0B3,
    0x0000B463,
    0x000002E3,
    0x00491023,
  };

  for(size_t i = 0; i < sizeof(program) / sizeof(program[0]); i++) {
    initialMemory[0x00001000 + static_cast<uint32_t>(i * 4)] = program[i];
  }

  TINY_RISC cpu("cpu", initialMemory, 10);

  sc_signal<bool> reset;
  cpu.reset(reset);
  reset.write(false);

  sc_start(500000, SC_NS);

  const uint32_t result = cpu.getMemoryValue(0x00000100);
  std::cout << "Memory[0x100] = " << result << std::endl;

  if(result != 55) {
    std::cerr << "Test failed: expected 55" << std::endl;
    return 1;
  }

  std::cout << "Test passed" << std::endl;
  return 0;
}
