#include "debug.hpp"

#include "decoder_cache.hpp"
#include "rv32i_instr.hpp"
#include "rv32i.hpp"
#include "rv64i.hpp"
#include "rv128i.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace riscv
{
static inline std::vector<std::string> split(const std::string& txt, char ch)
{
	size_t pos = txt.find(ch);
	size_t initialPos = 0;
	std::vector<std::string> strs;

	while (pos != std::string::npos)
	{
		strs.push_back(txt.substr(initialPos, pos - initialPos));
		initialPos = pos + 1;

		pos = txt.find(ch, initialPos);
	}

	// Add the last one
	strs.push_back(txt.substr(initialPos, std::min(pos, txt.size()) - initialPos + 1));
	return strs;
}
template <int W>
static void dprintf(CPU<W>& cpu, const char* fmt, ...)
{
	char buffer[2048];

	va_list args;
	va_start(args, fmt);
	int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	if (len > 0) {
		cpu.machine().debug_print(buffer, len);
	} else {
		throw MachineException(OUT_OF_MEMORY, "Debug print buffer too small");
	}
}

template <int W>
static void print_help(CPU<W>& cpu)
{
	const char* help_text = R"V0G0N(
  usage: command [options]
	commands:
	  ?, help               Show this informational text
	  q, quit               Exit the interactive debugger
	  c, continue           Continue execution, disable stepping
	  s, step [steps=1]     Run [steps] instructions, then break
	  b, break [addr]       Breakpoint when PC == addr
	  b, break [name]       Resolve symbol to addr, use as breakpoint
	  rb [addr]             Breakpoint on reading from [addr]
	  wb [addr]             Breakpoint on writing to [addr]
	  clear                 Clear all breakpoints
	  bt, backtrace         Display primitive backtrace
	  a, addrof [name]      Resolve symbol name to address (or 0x0)
	  read [addr] (len=1)   Read from [addr] (len) bytes and print
	  write [addr] [value]  Write [value] to memory location [addr]
	  print [addr] [length] Print [addr] as a string of [length] bytes
	  ebreak                Trigger the ebreak handler
	  syscall [num]         Trigger specific system call handler
	  v, verbose            Toggle verbose instruction output
	  vr, vregs             Toggle verbose register output
	  vf, vfpregs           Toggle verbose fp-register output
	  vj, vjumps            Toggle verbose jump output
)V0G0N";
	dprintf(cpu, "%s\n", help_text);
}

template <int W>
static bool execute_commands(DebugMachine<W>& debug)
{
	auto& machine = debug.machine;
	auto& cpu = machine.cpu;

	dprintf(cpu, "Enter = cont, help, quit: ");
	std::string text;
	while (true)
	{
		const int c = getchar(); // press any key
		if (c == '\n' || c < 0)
			break;
		else
			text.append(1, (char) c);
	}
	if (text.empty()) return false;
	std::vector<std::string> params = split(text, ' ');
	const auto& cmd = params[0];

	// continue
	if (cmd == "c" || cmd == "continue")
	{
		debug.break_on_steps(0);
		return false;
	}
	// stepping
	if (cmd == "")
	{
		return false;
	}
	else if (cmd == "s" || cmd == "step")
	{
		debug.verbose_instructions = true; // ???
		int steps = 1;
		if (params.size() > 1) steps = std::stoi(params[1]);
		dprintf(cpu, "Pressing Enter will now execute %d steps\n", steps);
		debug.break_on_steps(steps);
		return false;
	}
	// breaking
	else if (cmd == "b" || cmd == "break")
	{
		if (params.size() < 2)
		{
			dprintf(cpu, ">>> Not enough parameters: break [addr]\n");
			return true;
		}
		const auto addr = machine.address_of(params[1]);
		if (addr != 0x0) {
			dprintf(cpu, "Breakpoint on %s with address 0x%lX\n",
				params[1].c_str(), addr);
			debug.breakpoint(addr);
		} else {
			unsigned long hex = std::strtoul(params[1].c_str(), 0, 16);
			dprintf(cpu, "Breakpoint on address 0x%lX\n", hex);
			debug.breakpoint(hex);
		}
		return true;
	}
	else if (cmd == "clear")
	{
		debug.breakpoints().clear();
		return true;
	}
	else if (cmd == "bt" || cmd == "backtrace")
	{
		machine.memory.print_backtrace(
			[&cpu] (std::string_view line) {
				dprintf(cpu, "-> %.*s\n", (int)line.size(), line.begin());
			});
		return true;
	}
	else if (cmd == "a" || cmd == "addrof")
	{
		if (params.size() < 2)
		{
			dprintf(cpu, ">>> Not enough parameters: addrof [name]\n");
			return true;
		}
		const auto addr = machine.address_of(params[1]);
		dprintf(cpu, "The address of %s is 0x%lX.%s\n",
			params[1].c_str(), addr, addr == 0x0 ? " (Likely not found)" : "");
		return true;
	}
	// verbose instructions
	else if (cmd == "v" || cmd == "verbose")
	{
		bool& v = debug.verbose_instructions;
		v = !v;
		dprintf(cpu, "Verbose instructions are now %s\n", v ? "ON" : "OFF");
		return true;
	}
	else if (cmd == "vr" || cmd == "vregs")
	{
		bool& v = debug.verbose_registers;
		v = !v;
		dprintf(cpu, "Verbose registers are now %s\n", v ? "ON" : "OFF");
		return true;
	}
	else if (cmd == "vf" || cmd == "vfpregs")
	{
		bool& v = debug.verbose_fp_registers;
		v = !v;
		dprintf(cpu, "Verbose FP-registers are now %s\n", v ? "ON" : "OFF");
		return true;
	}
	else if (cmd == "vj" || cmd == "vjumps")
	{
		bool& v = debug.verbose_jumps;
		v = !v;
		dprintf(cpu, "Verbose jumps are now %s\n", v ? "ON" : "OFF");
		return true;
	}
	else if (cmd == "r" || cmd == "run")
	{
		debug.verbose_instructions = false;
		debug.break_on_steps(0);
		return false;
	}
	else if (cmd == "q" || cmd == "quit" || cmd == "exit")
	{
		machine.stop();
		return false;
	}
	// read 0xAddr size
	else if (cmd == "lw" || cmd == "read")
	{
		if (params.size() < 2)
		{
			dprintf(cpu, ">>> Not enough parameters: read [addr]\n");
			return true;
		}
		unsigned long addr = std::strtoul(params[1].c_str(), 0, 16);
		auto value = machine.memory.template read<uint32_t>(addr);
		dprintf(cpu, "0x%lX: 0x%X\n", addr, value);
		return true;
	}
	// write 0xAddr value
	else if (cmd == "sw" || cmd == "write")
	{
		if (params.size() < 3)
		{
			dprintf(cpu, ">>> Not enough parameters: write [addr] [value]\n");
			return true;
		}
		unsigned long hex = std::strtoul(params[1].c_str(), 0, 16);
		int value = std::stoi(params[2]) & 0xff;
		dprintf(cpu, "0x%04lx -> 0x%02x\n", hex, value);
		machine.memory.template write<uint32_t>(hex, value);
		return true;
	}
	// print 0xAddr size
	else if (cmd == "print")
	{
		if (params.size() < 3)
		{
			dprintf(cpu, ">>> Not enough parameters: print addr length\n");
			return true;
		}
		uint32_t src = std::strtoul(params[1].c_str(), 0, 16);
		int bytes = std::stoi(params[2]);
		std::unique_ptr<char[]> buffer(new char[bytes]);
		machine.memory.memcpy_out(buffer.get(), src, bytes);
		dprintf(cpu, "0x%X: %.*s\n", src, bytes, buffer.get());
		return true;
	}
	else if (cmd == "ebreak")
	{
		machine.system_call(SYSCALL_EBREAK);
		return true;
	}
	else if (cmd == "syscall")
	{
		int num = 0;
		if (params.size() > 1) num = std::stoi(params[1]);
		dprintf(cpu, "Triggering system call %d\n", num);
		machine.system_call(num);
		return true;
	}
	else if (cmd == "help" || cmd == "?")
	{
		print_help(cpu);
		return true;
	}
	else
	{
		dprintf(cpu, ">>> Unknown command: '%s'\n", cmd.c_str());
		print_help(cpu);
		return true;
	}
	return false;
}

template<int W>
void DebugMachine<W>::print_and_pause()
{
	auto& cpu = machine.cpu;
	try {
		const auto instruction = cpu.read_next_instruction();
		const auto& handler = cpu.decode(instruction);
		const auto string = isa_type<W>::to_string(cpu, instruction, handler);
		dprintf(cpu, "\n>>> Breakpoint \t%s\n\n", string.c_str());
	} catch (const std::exception& e) {
		dprintf(cpu, "\n>>> Breakpoint \tError reading instruction: %s\n\n", e.what());
	}
	// CPU registers
	dprintf(cpu, "%s", cpu.registers().to_string().c_str());
	// Memory subsystem
	dprintf(cpu, "[MEM PAGES     %8zu]\n", machine.memory.pages_active());
	// Floating-point registers
	if (this->verbose_fp_registers) {
		dprintf(cpu, "%s", cpu.registers().flp_to_string().c_str());
	}

	while (execute_commands(*this))
		;
} // print_and_pause(...)

template<int W>
bool DebugMachine<W>::break_time() const
{
	if (UNLIKELY(m_break_steps_cnt != 0))
	{
		m_break_steps--;
		if (m_break_steps <= 0)
		{
			m_break_steps = m_break_steps_cnt;
			return true;
		}
	}
	return false;
}

template<int W>
void DebugMachine<W>::break_on_steps(int steps)
{
	assert(steps >= 0);
	this->m_break_steps_cnt = steps;
	this->m_break_steps = steps;
}

template<int W>
void DebugMachine<W>::break_checks()
{
	if (UNLIKELY(this->break_time()))
	{
		// pause for each instruction
		this->print_and_pause();
	}
	if (UNLIKELY(!m_breakpoints.empty()))
	{
		// look for breakpoints
		auto it = m_breakpoints.find(machine.cpu.pc());
		if (it != m_breakpoints.end())
		{
			auto& callback = it->second;
			callback(*this);
		}
	}
}

template<int W>
void DebugMachine<W>::register_debug_logging() const
{
	auto regs = "\n" + machine.cpu.registers().to_string() + "\n\n";
	machine.debug_print(regs.data(), regs.size());
	if (UNLIKELY(this->verbose_fp_registers)) {
		regs = machine.cpu.registers().flp_to_string() + "\n";
		machine.debug_print(regs.data(), regs.size());
	}
}

// Instructions may be unaligned with C-extension
// On amd64 we take the cost, because it's faster
union UnderAlign32
{
	uint16_t data[2];
	operator uint32_t()
	{
		return data[0] | uint32_t(data[1]) << 16;
	}
};

#define INSTRUCTION_LOGGING()	\
	if (this->verbose_instructions) { \
		const auto string = isa_type<W>::to_string(cpu, instruction, cpu.decode(instruction)) + "\n"; \
		machine.print(string.c_str(), string.size()); \
	}

template<int W>
void DebugMachine<W>::simulate(uint64_t max)
{
	auto& cpu = machine.cpu;
	auto* exec = cpu.current_execute_segment();
	auto* exec_decoder = exec->decoder_cache();
	auto* exec_seg_data = exec->exec_data();

	// Calculate the instruction limit
	if (max != UINT64_MAX)
		machine.set_max_instructions(machine.instruction_counter() + max);
	else
		machine.set_max_instructions(UINT64_MAX);

	for (; machine.instruction_counter() < machine.max_instructions();
		machine.increment_counter(1)) {

		rv32i_instruction instruction;
		this->break_checks();

		// We can't use decoder cache when translator is enabled
		constexpr bool enable_cache = !binary_translation_enabled;
		if (cpu.is_executable(cpu.pc()))
		{
			auto pc = cpu.pc();

			// Instructions may be unaligned with C-extension
			instruction = rv32i_instruction { *(UnderAlign32*) &exec_seg_data[pc] };
			INSTRUCTION_LOGGING();

			if constexpr (enable_cache)
			{
				// Retrieve handler directly from the instruction handler cache
				auto& cache_entry =
					exec_decoder[pc / DecoderCache<W>::DIVISOR];
				cache_entry.execute(cpu, instruction);
			}
			else // Not the slowest path, since we have the instruction already
			{
				cpu.execute(instruction);
			}
		}
		else
		{
			instruction = cpu.read_next_instruction_slowpath();
			INSTRUCTION_LOGGING();
			// decode & execute instruction directly
			cpu.execute(instruction);
		}

		if (UNLIKELY(this->verbose_registers)) {
			this->register_debug_logging();
		}

		// increment PC
		if constexpr (compressed_enabled)
			cpu.registers().pc += instruction.length();
		else
			cpu.registers().pc += 4;
	} // while not stopped

} // DebugMachine::simulate


	template struct DebugMachine<4>;
	template struct DebugMachine<8>;
	template struct DebugMachine<16>;
} // riscv
