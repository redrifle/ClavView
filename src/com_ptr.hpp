#ifndef COM_PTR_HPP_019f8131_b40b_780e_8820_3bb985fd238d
#define COM_PTR_HPP_019f8131_b40b_780e_8820_3bb985fd238d
#include <windows.h>

// Minimal COM smart pointer. Deliberately avoids winrt::com_ptr so the
// project doesn't need to link windowsapp.lib -- this project only talks
// to classic COM interfaces (D2D, WIC), not the WinRT runtime.
template <typename T>
class com_ptr
{
	T* p_ {nullptr};

  public:
	com_ptr() = default;
	com_ptr(com_ptr const&) = delete;
	com_ptr& operator=(com_ptr const&) = delete;

	// Adopts a raw pointer without an extra AddRef -- use when ownership of
	// an already-AddRef'd interface is being transferred (e.g. across the
	// PostMessage thread boundary in the background decoder).
	static com_ptr attach(T* raw)
	{
		com_ptr p;
		p.p_ = raw;
		return p;
	}

	// Releases ownership without calling Release() -- pairs with attach()
	// when handing a pointer off to another owner (e.g. packing it into a
	// struct posted to another thread).
	T* detach()
	{
		T* tmp {p_};
		p_ = nullptr;
		return tmp;
	}

	com_ptr(com_ptr&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }

	com_ptr& operator=(com_ptr&& other) noexcept
	{
		if (this != &other)
		{
			reset();
			p_ = other.p_;
			other.p_ = nullptr;
		}
		return *this;
	}

	~com_ptr() { reset(); }

	void reset()
	{
		if (p_)
		{
			p_->Release();
			p_ = nullptr;
		}
	}

	// Out-parameter access for Create*/Initialize-style APIs. Releases any
	// existing pointer first so re-using a com_ptr for a second call is safe.
	T** put()
	{
		reset();
		return &p_;
	}

	// Out-parameter access as void** for QueryInterface-style APIs.
	void** put_void() { return reinterpret_cast<void**>(put()); }

	T* get() const { return p_; }
	T* operator->() const { return p_; }
	explicit operator bool() const { return p_ != nullptr; }
};

#endif /* COM_PTR_HPP_019f8131_b40b_780e_8820_3bb985fd238d */
