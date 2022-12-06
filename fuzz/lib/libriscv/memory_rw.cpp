#include "machine.hpp"

namespace riscv
{
	template <int W>
	const Page& Memory<W>::get_readable_pageno(const address_t pageno) const
	{
		const auto& page = get_pageno(pageno);
		if (LIKELY(page.attr.read))
			return page;
		this->protection_fault(pageno * Page::size());
	}

	template <int W>
	Page& Memory<W>::create_writable_pageno(const address_t pageno, bool init)
	{
		auto it = m_pages.find(pageno);
		if (it != m_pages.end()) {
			Page& page = it->second;
			if (LIKELY(page.attr.write)) {
				return page;
			} else if (page.attr.is_cow) {
				m_page_write_handler(*this, pageno, page);
				return page;
			}
		} else {
		#ifdef RISCV_RODATA_SEGMENT_IS_SHARED
			if (UNLIKELY(m_ropages.contains(pageno))) {
				this->protection_fault(pageno * Page::size());
			}
		#endif
			// Handler must produce a new page, or throw
			Page& page = m_page_fault_handler(*this, pageno,
				init ? PageData::INITIALIZED : PageData::UNINITIALIZED);
			if (LIKELY(page.attr.write))
				return page;
		}
		this->protection_fault(pageno * Page::size());
	}

	template <int W>
	void Memory<W>::free_pages(address_t dst, size_t len)
	{
		address_t pageno = page_number(dst);
		address_t end = pageno + (len /= Page::size());
		while (pageno < end)
		{
			auto it = m_pages.find(pageno);
			if (it != m_pages.end()) {
				m_pages.erase(it);
			}
			pageno ++;
		}
		// TODO: This can be improved by invalidating matches only
		this->invalidate_reset_cache();
	}

	template <int W>
	void Memory<W>::default_page_write(Memory<W>&, address_t, Page& page)
	{
		page.make_writable();
	}

	template <int W>
	const Page& Memory<W>::default_page_read(const Memory<W>&, address_t)
	{
		return Page::cow_page();
	}

	static const Page zeroed_page {
		PageAttributes {
			.read   = true,
			.write  = false,
			.exec   = false,
			.is_cow = true
		}
	};
	static const Page guarded_page {
		PageAttributes {
			.read   = false,
			.write  = false,
			.exec   = false,
			.is_cow = false,
			.non_owning = true
		}, nullptr
	};
	const Page& Page::cow_page() noexcept {
		return zeroed_page; // read-only, zeroed page
	}
	const Page& Page::guard_page() noexcept {
		return guarded_page; // inaccessible page
	}

	template <int W>
	Page& Memory<W>::install_shared_page(address_t pageno, const Page& shared_page)
	{
		auto& already_there = get_pageno(pageno);
		if (!already_there.is_cow_page() && !already_there.attr.non_owning)
			throw MachineException(ILLEGAL_OPERATION,
				"There was a page at the specified location already", pageno);
		if (shared_page.data() == nullptr && (
			shared_page.attr.write || shared_page.attr.read || shared_page.attr.exec))
			throw MachineException(ILLEGAL_OPERATION,
				"There was a RWX page with no allocated data", pageno);

		auto attr = shared_page.attr;
		attr.non_owning = true;
		// NOTE: If you insert a const Page, DON'T modify it! The machine
		// won't, unless system-calls do or manual intervention happens!
		auto res = m_pages.emplace(std::piecewise_construct,
			std::forward_as_tuple(pageno),
			std::forward_as_tuple(attr, const_cast<PageData*> (shared_page.m_page.get()))
		);
		// TODO: Can be improved by invalidating more intelligently
		this->invalidate_reset_cache();
		// try overwriting instead, if emplace failed
		if (res.second == false) {
			Page& page = res.first->second;
			new (&page) Page{attr, const_cast<PageData*> (shared_page.m_page.get())};
			return page;
		}
		return res.first->second;
	}

	template <int W>
	void Memory<W>::insert_non_owned_memory(
		address_t dst, void* src, size_t size, PageAttributes attr)
	{
		assert(dst % Page::size() == 0);
		assert((dst + size) % Page::size() == 0);
		attr.non_owning = true;

		for (size_t i = 0; i < size; i += Page::size())
		{
			const auto pageno = (dst + i) >> Page::SHIFT;
			PageData* pdata = reinterpret_cast<PageData*> ((char*) src + i);
			m_pages.emplace(std::piecewise_construct,
				std::forward_as_tuple(pageno),
				std::forward_as_tuple(attr, pdata)
			);
		}
		// TODO: Can be improved by invalidating more intelligently
		this->invalidate_reset_cache();
	}

	template <int W> void
	Memory<W>::set_page_attr(address_t dst, size_t len, PageAttributes options)
	{
		const bool is_default = options.is_default();
		while (len > 0)
		{
			const size_t size = std::min(Page::size(), len);
			const address_t pageno = page_number(dst);
			auto it = pages().find(pageno);
			if (it != pages().end()) {
				auto& page = it->second;
				if (page.is_cow_page()) {
					// The special zero-CoW page is an internal optimization
					// We can ignore the page if the default attrs apply.
					if (!is_default)
						this->create_writable_pageno(pageno).attr = options;
				} else {
					// There is a page there
					page.attr = options;
				}
			} else {
				// If the page was not found, it was likely (also) the
				// special zero-CoW page.
				if (!is_default)
					this->create_writable_pageno(pageno).attr = options;
			}

			dst += size;
			len -= size;
		}
	}

	template struct Memory<4>;
	template struct Memory<8>;
	template struct Memory<16>;
}
