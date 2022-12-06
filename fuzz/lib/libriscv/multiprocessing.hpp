#pragma once
#include "util/threadpool.h"

namespace riscv {

template <int W>
struct Multiprocessing
{
	using failure_bits_t = uint32_t;

	Multiprocessing(size_t);
#ifdef RISCV_MULTIPROCESS
	void async_work(std::function<void()>&& wrk);
	failure_bits_t wait();
	bool is_multiprocessing() const noexcept { return this->processing; }

	ThreadPool m_threadpool;
	std::mutex m_lock;
	bool processing = false;
	failure_bits_t failures = 0; // Bitmap of failed vCPU tasks
	static constexpr bool shared_page_faults = false;
	static constexpr bool shared_read_faults = false;
#else
	bool is_multiprocessing() const noexcept { return false; }
#endif
};

} // riscv
