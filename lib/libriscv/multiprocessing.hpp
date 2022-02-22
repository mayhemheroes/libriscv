#pragma once
#include "util/threadpool.h"

namespace riscv {

template <int W>
struct Multiprocessing
{
#ifdef RISCV_MULTIPROCESS
	void async_work(std::function<void()> wrk);
	void wait();
	bool is_multiprocessing() const noexcept { return this->processing; }

	ThreadPool m_threadpool;
	std::mutex m_lock;
	bool processing = false;
#else
	bool is_multiprocessing() const noexcept { return false; }
#endif
};

} // riscv
