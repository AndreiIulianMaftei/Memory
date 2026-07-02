#ifndef TINYRISC_HPP
#define TINYRISC_HPP

#include <systemc>
#include <systemc.h>
#include <cstdint>
#include <map>
using namespace sc_core;

#include "cu.hpp"
#include "main_memory.hpp"
#include "register_file.hpp"
#include "alu.hpp"
#include "multiplexer.hpp"
#include "pc.hpp"

SC_MODULE(TINY_RISC) {
  sc_in<bool> reset;

  std::map<uint32_t, uint32_t> initialMemory;

  sc_clock clk;

  REGISTER_FILE registerFile;
  MAIN_MEMORY mainMemory;
  ALU alu;
  PC pc;
  CU cu;

  MULTIPLEXER_I32 muxReg1;
  MULTIPLEXER_I32 muxReg2;
  MULTIPLEXER_I32 muxAluRo;
  MULTIPLEXER_I32 muxRegW;
  MULTIPLEXER_I32 muxMemR;
  MULTIPLEXER_I32 muxMemAddr;

  sc_signal<uint32_t> registerR1;
  sc_signal<uint32_t> registerR2;
  sc_signal<uint32_t> registerR1FromModule;
  sc_signal<uint32_t> registerR2FromModule;
  sc_signal<uint32_t> aluR1;
  sc_signal<uint32_t> aluR2;
  sc_signal<uint32_t> pcNext;
  sc_signal<uint32_t> memoryAddressFromRegister;
  sc_signal<uint32_t> memoryWriteData;
  sc_signal<uint32_t> aluRo;
  sc_signal<uint32_t> registerWriteDataFromAlu;
  sc_signal<uint32_t> cuRo;
  sc_signal<uint32_t> registerWriteData;
  sc_signal<uint32_t> memoryReadData;
  sc_signal<uint32_t> cuDataIn;
  sc_signal<uint32_t> registerWriteDataFromMemory;
  sc_signal<uint32_t> memoryAddress;
  sc_signal<uint32_t> cuAddress;
  sc_signal<uint32_t> pcOut;

  sc_signal<bool> aluOverflow;
  sc_signal<bool> memoryReady;
  sc_signal<bool> pcEnable;
  sc_signal<bool> readMemory;
  sc_signal<bool> writeMemory;
  sc_signal<bool> registerWriteEnable;

  sc_signal<sc_bv<2>> pcJump;
  sc_signal<sc_bv<12>> pcOffset;
  sc_signal<sc_bv<3>> aluOp;
  sc_signal<sc_bv<5>> registerAddress1;
  sc_signal<sc_bv<5>> registerAddress2;
  sc_signal<sc_bv<5>> registerAddress3;

  sc_signal<uint8_t> muxReg1Select;
  sc_signal<uint8_t> muxReg2Select;
  sc_signal<uint8_t> muxAluRoSelect;
  sc_signal<uint8_t> muxMemRSelect;
  sc_signal<uint8_t> registerWriteSource;
  sc_signal<uint8_t> memoryAddressSource;

  SC_HAS_PROCESS(TINY_RISC);

  TINY_RISC(sc_module_name name, std::map<uint32_t, uint32_t> initialMemory, uint32_t period)
    : sc_module(name),
      reset("reset"),
      initialMemory(initialMemory),
      clk("clk", period, SC_NS),
      registerFile("registerFile"),
      mainMemory("mainMemory"),
      alu("alu"),
      pc("pc"),
      cu("cu"),
      muxReg1("muxReg1", 1, 3),
      muxReg2("muxReg2", 1, 2),
      muxAluRo("muxAluRo", 1, 2),
      muxRegW("muxRegW", 2, 1),
      muxMemR("muxMemR", 1, 2),
      muxMemAddr("muxMemAddr", 2, 1) {
    bindComponents();
    resetState();

    muxReg1Select.write(0);
    muxReg2Select.write(0);
    muxAluRoSelect.write(0);
    muxMemRSelect.write(0);

    SC_THREAD(resetBehaviour);
    sensitive << clk.posedge_event();

    SC_THREAD(registerOutputBehaviour);
  }

  uint32_t getMemoryValue(uint32_t address) {
    return mainMemory.get(address);
  }

  void bindComponents() {
    registerFile.clk(clk);
    registerFile.a1(registerAddress1);
    registerFile.a2(registerAddress2);
    registerFile.a3(registerAddress3);
    registerFile.we(registerWriteEnable);
    registerFile.wdata(registerWriteData);
    registerFile.r1(registerR1FromModule);
    registerFile.r2(registerR2FromModule);

    mainMemory.clk(clk);
    mainMemory.addr(memoryAddress);
    mainMemory.wdata(memoryWriteData);
    mainMemory.r(readMemory);
    mainMemory.w(writeMemory);
    mainMemory.rdata(memoryReadData);
    mainMemory.ready(memoryReady);

    alu.r1(aluR1);
    alu.r2(aluR2);
    alu.op(aluOp);
    alu.ro(aluRo);
    alu.overflow(aluOverflow);

    pc.clk(clk);
    pc.enable(pcEnable);
    pc.j(pcJump);
    pc.next(pcNext);
    pc.offset(pcOffset);
    pc.pcout(pcOut);

    cu.clk(clk);
    cu.pcin(pcOut);
    cu.datain(cuDataIn);
    cu.memready(memoryReady);
    cu.ro(cuRo);
    cu.overflow(aluOverflow);
    cu.pcenable(pcEnable);
    cu.pcj(pcJump);
    cu.pcoffset(pcOffset);
    cu.addr(cuAddress);
    cu.rmem(readMemory);
    cu.wmem(writeMemory);
    cu.op(aluOp);
    cu.a1(registerAddress1);
    cu.a2(registerAddress2);
    cu.a3(registerAddress3);
    cu.we(registerWriteEnable);
    cu.rsource(registerWriteSource);
    cu.msource(memoryAddressSource);

    muxReg1.in[0](registerR1);
    muxReg1.select(muxReg1Select);
    muxReg1.out[0](aluR1);
    muxReg1.out[1](pcNext);
    muxReg1.out[2](memoryAddressFromRegister);

    muxReg2.in[0](registerR2);
    muxReg2.select(muxReg2Select);
    muxReg2.out[0](aluR2);
    muxReg2.out[1](memoryWriteData);

    muxAluRo.in[0](aluRo);
    muxAluRo.select(muxAluRoSelect);
    muxAluRo.out[0](registerWriteDataFromAlu);
    muxAluRo.out[1](cuRo);

    muxRegW.in[0](registerWriteDataFromAlu);
    muxRegW.in[1](registerWriteDataFromMemory);
    muxRegW.select(registerWriteSource);
    muxRegW.out[0](registerWriteData);

    muxMemR.in[0](memoryReadData);
    muxMemR.select(muxMemRSelect);
    muxMemR.out[0](cuDataIn);
    muxMemR.out[1](registerWriteDataFromMemory);

    muxMemAddr.in[0](memoryAddressFromRegister);
    muxMemAddr.in[1](cuAddress);
    muxMemAddr.select(memoryAddressSource);
    muxMemAddr.out[0](memoryAddress);
  }

  void resetBehaviour() {
    while(true) {
      wait();
      if(reset.read()) {
        resetState();
      }
    }
  }

  void registerOutputBehaviour() {
    while(true) {
      wait(registerAddress1.value_changed_event() | registerAddress2.value_changed_event() | clk.posedge_event());
      wait(SC_ZERO_TIME);

      uint8_t address1 = registerAddress1.read().to_uint();
      uint8_t address2 = registerAddress2.read().to_uint();

      registerR1.write(address1 != 0 ? registerFile.registers_data[address1] : 0);
      registerR2.write(address2 != 0 ? registerFile.registers_data[address2] : 0);
    }
  }

  void resetState() {
    pc.pcValue = 0x00001000;

    for(int i = 0; i < 32; i++) {
      registerFile.registers_data[i] = 0;
    }

    mainMemory.memory.clear();
    for(std::map<uint32_t, uint32_t>::const_iterator it = initialMemory.begin(); it != initialMemory.end(); ++it) {
      mainMemory.set(it->first, it->second);
    }
  }
};


#endif // TINYRISC_HPP
