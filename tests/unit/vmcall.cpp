#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libriscv/machine.hpp>
extern std::vector<uint8_t> build_and_load(const std::string& code,
	const std::string& args = "-O2 -static -Wl,--undefined=hello", bool cpp = false);
static const uint64_t MAX_MEMORY = 8ul << 20; /* 8MB */
static const uint64_t MAX_INSTRUCTIONS = 10'000'000ul;
using namespace riscv;

TEST_CASE("VM function call", "[VMCall]")
{
	struct State {
		bool output_is_hello_world = false;
	} state;
	const auto binary = build_and_load(R"M(
	extern long write(int, const void*, unsigned long);
	extern void hello() {
		write(1, "Hello World!", 12);
	}
	
	int main() {
		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
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
	// Run for at most X instructions before giving up
	machine.simulate(MAX_INSTRUCTIONS);

	REQUIRE(machine.return_value<int>() == 666);
	REQUIRE(!state.output_is_hello_world);

	const auto hello_address = machine.address_of("hello");
	REQUIRE(hello_address != 0x0);

	// Execute guest function
	machine.vmcall(hello_address);

	// Now hello world should have been printed
	REQUIRE(state.output_is_hello_world);
}

TEST_CASE("VM function call in fork", "[VMCall]")
{
	// The global variable 'value' should get
	// forked as value=1. We assert this, then
	// we set value=0. New forks should continue
	// to see value=1 as they are forked from the
	// main VM where value is still 0.
	const auto binary = build_and_load(R"M(
	#include <assert.h>
	#include <string.h>
	extern long write(int, const void*, unsigned long);
	static int value = 0;

	extern void hello() {
		assert(value == 1);
		value = 0;
		write(1, "Hello World!", 12);
	}

	extern int str(const char *arg) {
		assert(strcmp(arg, "Hello") == 0);
		return 1;
	}

	struct Data {
		int val1;
		int val2;
		float f1;
	};
	extern int structs(struct Data *data) {
		assert(data->val1 == 1);
		assert(data->val2 == 2);
		assert(data->f1 == 3.0f);
		return 2;
	}

	extern int ints(long i1, long i2, long i3) {
		assert(i1 == 123);
		assert(i2 == 456);
		assert(i3 == 456);
		return 3;
	}

	int main() {
		value = 1;
		return 666;
	})M");

	riscv::Machine<RISCV64> machine { binary, { .memory_max = MAX_MEMORY } };
	machine.setup_linux_syscalls();
	machine.setup_linux(
		{"vmcall"},
		{"LC_TYPE=C", "LC_ALL=C", "USER=root"});

	machine.simulate(MAX_INSTRUCTIONS);
	REQUIRE(machine.return_value<int>() == 666);

	// Test many forks
	for (size_t i = 0; i < 10; i++)
	{
		riscv::Machine<RISCV64> fork { machine };

		fork.set_printer([] (const auto&, const char* data, size_t size) {
			std::string text{data, data + size};
			REQUIRE(text == "Hello World!");
		});

		const auto hello_address = fork.address_of("hello");
		REQUIRE(hello_address != 0x0);

		// Execute guest function
		fork.vmcall(hello_address);

		int res1 = fork.vmcall("str", "Hello");
		REQUIRE(res1 == 1);

		res1 = fork.vmcall("str", std::string("Hello"));
		REQUIRE(res1 == 1);

		struct {
			int v1 = 1;
			int v2 = 2;
			float f1 = 3.0f;
		} data;
		int res2 = fork.vmcall("structs", data);
		REQUIRE(res2 == 2);

		long intval = 456;
		long& intref = intval;

		int res3 = fork.vmcall("ints", 123L, intref, (long&&)intref);
		REQUIRE(res3 == 3);
	}
}
