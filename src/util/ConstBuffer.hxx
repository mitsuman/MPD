/*
 * Copyright (C) 2013 Max Kellermann <max@duempel.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef CONST_BUFFER_HPP
#define CONST_BUFFER_HPP

#include "Compiler.h"

#include <cstddef>

#ifndef NDEBUG
#include <assert.h>
#endif

/**
 * A reference to a memory area that is read-only.
 */
template<typename T>
struct ConstBuffer {
	typedef size_t size_type;
	typedef const T *pointer_type;
	typedef pointer_type const_pointer_type;
	typedef pointer_type iterator;
	typedef pointer_type const_iterator;

	pointer_type data;
	size_type size;

	ConstBuffer() = default;

	constexpr ConstBuffer(std::nullptr_t):data(nullptr), size(0) {}

	constexpr ConstBuffer(pointer_type _data, size_type _size)
		:data(_data), size(_size) {}

	constexpr static ConstBuffer Null() {
		return ConstBuffer(nullptr, 0);
	}

	/**
	 * Cast a ConstBuffer<void> to a ConstBuffer<T>.  A "void"
	 * buffer records its size in bytes, and when casting to "T",
	 * the assertion below ensures that the size is a multiple of
	 * sizeof(T).
	 */
#ifdef NDEBUG
	constexpr
#endif
	static ConstBuffer<T> FromVoid(ConstBuffer<void> other) {
		static_assert(sizeof(T) > 0, "Empty base type");
#ifndef NDEBUG
		assert(other.size % sizeof(T) == 0);
#endif
		return ConstBuffer<T>(pointer_type(other.data),
				      other.size / sizeof(T));
	}

	constexpr ConstBuffer<void> ToVoid() const {
		static_assert(sizeof(T) > 0, "Empty base type");
		return ConstBuffer<void>(data, size * sizeof(T));
	}

	constexpr bool IsNull() const {
		return data == nullptr;
	}

	constexpr bool IsEmpty() const {
		return size == 0;
	}

	constexpr iterator begin() const {
		return data;
	}

	constexpr iterator end() const {
		return data + size;
	}

	constexpr const_iterator cbegin() const {
		return data;
	}

	constexpr const_iterator cend() const {
		return data + size;
	}

	constexpr operator ConstBuffer<void>() const {
		return { data, size };
	}
};

#endif
