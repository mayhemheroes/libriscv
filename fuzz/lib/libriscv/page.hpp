#pragma once
#include "common.hpp"
#include "types.hpp"
#include <cassert>
#include <memory>
#include <array>

namespace riscv {

struct PageAttributes
{
	bool read = true;
	bool write = true;
	bool exec = false;
	bool is_cow = false;
	bool non_owning = false;
	bool dont_fork = false;
	bool cacheable = true;
	uint8_t user_defined = 0; /* Use this for yourself */

	constexpr bool is_cacheable() const noexcept {
		// Cacheable only makes sense when memory traps are enabled
		if constexpr (memory_traps_enabled)
			return cacheable;
		else
			return true;
	}
	bool is_default() const noexcept {
		constexpr PageAttributes def {};
		return this->read == def.read && this->write == def.write && this->exec == def.exec;
	}
};

struct alignas(8) PageData {
	std::array<uint8_t, PageSize> buffer8;

	template <typename T>
	inline T& aligned_read(uint32_t offset) const
	{
		if constexpr (memory_alignment_check) {
			if (offset % sizeof(T))
				throw MachineException(INVALID_ALIGNMENT, "Misaligned read", offset);
		}
		return *(T*) &buffer8[offset];
	}

	template <typename T>
	inline void aligned_write(uint32_t offset, T value)
	{
		if constexpr (memory_alignment_check) {
			if (offset % sizeof(T))
				throw MachineException(INVALID_ALIGNMENT, "Misaligned write", offset);
		}
		*(T*) &buffer8[offset] = value;
	}

	PageData() : buffer8{} {}
	PageData(const PageData& other) : buffer8{other.buffer8} {}
	enum Initialization { INITIALIZED, UNINITIALIZED };
	PageData(Initialization i) { if (i == INITIALIZED) buffer8 = {}; }
};

struct Page
{
	static constexpr unsigned SIZE  = PageSize;
	static constexpr unsigned SHIFT = 31 - __builtin_clz(PageSize);
	static_assert((1u << SHIFT) == PageSize, "Page shift value must match page size");

	using mmio_cb_t = std::function<void(Page&, uint32_t, int, int64_t)>;

	// create a new blank page
	Page() { m_page.reset(new PageData {}); };
	// create a new possibly uninitialized page
	Page(PageData::Initialization i) { m_page.reset(new PageData {i}); };
	// copy another page (or data)
	Page(const PageAttributes& a, const PageData& d = {})
		: attr(a), m_page(new PageData{d}) { attr.non_owning = false; }
	Page(Page&& other)
		: attr(other.attr), m_page(std::move(other.m_page)) {}
	Page& operator= (Page&& other) {
		attr = other.attr;
		m_page = std::move(other.m_page);
		return *this;
	}
	// create a page that doesn't own this memory
	Page(const PageAttributes& a, PageData* data);
	// don't try to free non-owned page memory
	~Page() {
		if (attr.non_owning) m_page.release();
	}

	bool has_data() const noexcept { return m_page != nullptr; }
	auto& page() noexcept { return *m_page; }
	const auto& page() const noexcept { return *m_page; }

	std::string to_string() const;

	auto* data() noexcept {
		return page().buffer8.data();
	}
	const auto* data() const noexcept {
		return page().buffer8.data();
	}

	static constexpr size_t size() noexcept {
		return SIZE;
	}

	bool is_cow_page() const noexcept { return this == &cow_page(); }

	static const Page& cow_page() noexcept;
	static const Page& guard_page() noexcept;

	/* Transform a CoW-page to an owned writable page */
	void make_writable()
	{
		if (m_page != nullptr)
		{
			auto* new_data = new PageData {*m_page};
			if (attr.non_owning) m_page.release();
			m_page.reset(new_data);
		} else {
			m_page.reset(new PageData {});
		}
		attr.write = true;
		attr.is_cow = false;
		attr.non_owning = false;
	}

	// this combination has been benchmarked to be faster than
	// page-aligning the PageData struct and putting it first
	PageAttributes attr;
	std::unique_ptr<PageData> m_page;

	bool has_trap() const noexcept { return m_trap != nullptr; }
	// NOTE: Setting a trap makes the page uncacheable
	void set_trap(mmio_cb_t newtrap) const;
	void trap(uint32_t offset, int mode, int64_t value) const;
	static int trap_mode(int mode) noexcept { return mode & 0xF000; }

	mutable mmio_cb_t m_trap = nullptr;
};

inline Page::Page(const PageAttributes& a, PageData* data)
	: attr(a)
{
	attr.non_owning = true;
	m_page.reset(data);
}

inline void Page::trap(uint32_t offset, int mode, int64_t value) const
{
	this->m_trap((Page&) *this, offset, mode, value);
}
inline void Page::set_trap(mmio_cb_t newtrap) const {
#  ifdef RISCV_MEMORY_TRAPS
	this->attr.cacheable = false;
	this->m_trap = newtrap;
#  else
	(void) newtrap;
	throw MachineException(FEATURE_DISABLED, "Memory traps have not been enabled");
#  endif
}

inline std::string Page::to_string() const
{
	return "Readable: " + std::string(attr.read ? "[x]" : "[ ]") +
		"  Writable: " + std::string(attr.write ? "[x]" : "[ ]") +
		"  Executable: " + std::string(attr.exec ? "[x]" : "[ ]");
}

// Helper class for caching pages
template <int W, typename T> struct CachedPage {
	address_type<W> pageno = (address_type<W>)-1;
	T* page = nullptr;

	void reset() { pageno = (address_type<W>)-1; page = nullptr; }
};

}
