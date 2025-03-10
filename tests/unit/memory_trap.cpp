#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static -Wl,--undefined=hello", bool cpp = false);
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
using namespace riscv;

TEST_CASE("Read and write traps", "[Memory Traps]")
{
	struct State {
		bool output_is_hello_world = false;
	} state;
	const auto binary = build_and_load(R"M(
	extern void hello_write() {
		*(long *)0xF0000000 = 1234;
	}
	extern long hello_read() {
		return *(long *)0xF0000000;
	}

	int main() {
		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary };
	// We need to install Linux system calls for maximum gucciness
	machine.setup_linux_syscalls();
	// We need to create a Linux environment for runtimes to work well
	machine.setup_linux(
		{"vmcall"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	machine.set_userdata(&state);
	machine.set_printer([] (const auto& m, const char* data, size_t size) {
		auto* state = m.template get_userdata<State> ();
		std::string text{data, data + size};
		state->output_is_hello_world = (text == "Hello World!");
	});
	constexpr uint64_t TRAP_PAGE = 0xF0000000;
	bool trapped_write = false;
	bool trapped_read  = false;

	auto& trap_page =
		machine.memory.create_writable_pageno(Memory<RISCV64>::page_number(TRAP_PAGE));
	trap_page.set_trap(
		[&] (auto&, uint32_t /*offset*/, int mode, int64_t value) {
			switch (Page::trap_mode(mode))
			{
			case TRAP_WRITE:
				REQUIRE(value == 1234);
				trapped_write = true;
				break;
			case TRAP_READ:
				REQUIRE(value == 0);
				trapped_read = true;
				break;
			}
		});

	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value<int>() == 666);
	REQUIRE(trapped_read  == false);
	REQUIRE(trapped_write == false);

	machine.vmcall("hello_write");
	REQUIRE(trapped_write == true);
	REQUIRE(trapped_read  == false);

	machine.vmcall("hello_read");
	REQUIRE(trapped_write == true);
	REQUIRE(trapped_read  == true);
}

TEST_CASE("Execute traps", "[Memory Traps]")
{
	const auto binary = build_and_load(R"M(
	static void (*other_exit)() = (void(*)()) 0xF0000000;
	extern void _exit(int);

	int main() {
		other_exit();
		_exit(1234);
	})M");

	riscv::Machine<RISCV64> machine { binary };
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"vmcall"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	constexpr uint64_t TRAP_PAGE = 0xF0000000;

	// Install exit(666) code at TRAP_PAGE
	static const std::array<uint32_t, 3> dont_execute_this {
		0x29a00513, //        li      a0,666
		0x05d00893, //        li      a7,93
		0x00000073, //        ecall
	};
	machine.copy_to_guest(TRAP_PAGE, dont_execute_this.data(), 12);

	auto& trap_page =
		machine.memory.create_writable_pageno(Memory<RISCV64>::page_number(TRAP_PAGE));
	trap_page.attr.exec  = true;
	trap_page.attr.read  = false;
	trap_page.attr.write = false;

	bool trapped_exec = false;

	trap_page.set_trap(
		[&] (auto&, uint32_t offset, int mode, int64_t value) {
			switch (Page::trap_mode(mode))
			{
			case TRAP_EXEC:
				REQUIRE(offset == 0x0);
				REQUIRE(value == TRAP_PAGE);
				trapped_exec = true;
				// Return to caller
				machine.cpu.jump(machine.cpu.reg(riscv::REG_RA));
				break;
			default:
				throw std::runtime_error("Nope");
				break;
			}
		});

	// Using _exit we can run this test in a loop
	const auto main_addr = machine.address_of("main");

	for (size_t i = 0; i < 15; i++)
	{
		machine.cpu.jump(main_addr);
		trapped_exec = false;
		machine.simulate(MAX_INSTRUCTIONS);

		REQUIRE(machine.return_value<int>() == 1234);
		REQUIRE(trapped_exec  == true);
	}
}

TEST_CASE("Override execute space protection fault", "[Memory Traps]")
{
	struct State {
		bool trapped_fault = false;
		const uint64_t TRAP_ADDR = 0xF0000000;
	} state;
	const auto binary = build_and_load(R"M(
	static void (*other_exit)() = (void(*)()) 0xF0000000;
	extern void _exit(int);

	int main() {
		other_exit();
		_exit(1234);
	})M");

	riscv::Machine<RISCV64> machine { binary };
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"vmcall"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	machine.set_userdata(&state);
	machine.cpu.set_fault_handler([] (auto& cpu, auto&) {
		auto& state = *cpu.machine().template get_userdata<State> ();
		// We can successfully handle an execute space protection
		// fault by returning back to caller.
		if (cpu.pc() == state.TRAP_ADDR)
		{
			state.trapped_fault = true;
			// Return to caller
			cpu.jump(cpu.reg(riscv::REG_RA));
			return;
		}
		// CPU is not where we wanted
		cpu.trigger_exception(riscv::EXECUTION_SPACE_PROTECTION_FAULT, cpu.pc());
	});

	// Install exit(666) code at TRAP_PAGE
	static const std::array<uint32_t, 3> dont_execute_this {
		0x29a00513, //        li      a0,666
		0x05d00893, //        li      a7,93
		0x00000073, //        ecall
	};
	machine.copy_to_guest(state.TRAP_ADDR, dont_execute_this.data(), 12);

	// Make sure the page is not executable
	auto& trap_page =
		machine.memory.create_writable_pageno(Memory<RISCV64>::page_number(state.TRAP_ADDR));
	trap_page.attr.exec  = false;

	// Using _exit we can run this test in a loop
	const auto main_addr = machine.address_of("main");

	for (size_t i = 0; i < 15; i++)
	{
		machine.cpu.jump(main_addr);
		state.trapped_fault = false;
		machine.simulate(MAX_INSTRUCTIONS);

		REQUIRE(machine.return_value<int>() == 1234);
		REQUIRE(state.trapped_fault == true);
	}
}
