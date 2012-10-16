#pragma once
/**
    @file
    @brief TagMultiGr
    @author MITSUNARI Shigeo(@herumi)
	@license modified new BSD license
	http://www.opensource.org/licenses/bsd-license.php
*/
#include <assert.h>

namespace mie {

// default tag is for multiplicative group
template<class G>
struct TagMultiGr {
	static void square(G& z, const G& x)
	{
		G::mul(z, z, x);
	}
	static void mul(G& z, const G& x, const G& y)
	{
		G::mul(z, x, y);
	}
	static void div(G& z, const G& x, const G& y)
	{
		G::div(z, x, y);
	}
	static void init(G& x)
	{
		x = 1;
	}
};

} // mie

