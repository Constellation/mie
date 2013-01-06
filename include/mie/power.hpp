#pragma once
/**
	@file
	@brief power
	@author MITSUNARI Shigeo(@herumi)
	@license modified new BSD license
	http://opensource.org/licenses/BSD-3-Clause
*/
#include <assert.h>
#include <cybozu/inttype.hpp>
#include <mie/tagmultigr.hpp>

namespace mie {

namespace power_impl {

template<class F>
struct TagInt {
	typedef typename F::BlockType BlockType;
	static size_t getBlockSize(const F& n)
	{
		return F::getBlockSize(n);
	}
	static BlockType getBlock(const F& n, size_t i)
	{
		return F::getBlock(n, i);
	}
};

template<>
struct TagInt<int> {
	typedef int BlockType;
	static int getBlockSize(int)
	{
		return 1;
	}
	static BlockType getBlock(int n, size_t i)
	{
		assert(i == 0);
		cybozu::disable_warning_unused_variable(i);
		return n;
	}
};

template<>
struct TagInt<size_t> {
	typedef size_t BlockType;
	static size_t getBlockSize(size_t)
	{
		return 1;
	}
	static BlockType getBlock(size_t n, size_t i)
	{
		assert(i == 0);
		cybozu::disable_warning_unused_variable(i);
		return n;
	}
};

template<class G, class F>
void power(G& z, const G& x, const F& _y)
{
	typedef TagMultiGr<G> TagG;
	typedef power_impl::TagInt<F> TagI;
	typedef typename TagI::BlockType BlockType;
	if (_y == 0) {
		TagG::init(z);
		return;
	}
	if (_y == 1) {
		z = x;
		return;
	}
	bool isNegative = _y < 0;
	const F& y = isNegative ? -_y : _y;
	G out;
	TagG::init(out);
	G t(x);
	for (size_t i = 0, n = TagI::getBlockSize(y); i < n; i++) {
		BlockType v = TagI::getBlock(y, i);
		int m = (int)sizeof(BlockType) * 8;
		if (i == n - 1) {
			// avoid unused multiplication
			while (m > 0 && (v & (BlockType(1) << (m - 1))) == 0) {
				m--;
			}
		}
		for (int j = 0; j < m; j++) {
			if (v & (BlockType(1) << j)) {
				TagG::mul(out, out, t);
			}
			TagG::square(t, t);
		}
	}
	z = out;
	if (isNegative) {
		TagG::inv(z, z);
	}
}

} } // mie::power_impl

