#include <cybozu/test.hpp>
#include <cybozu/benchmark.hpp>
#include <mie/fp.hpp>
#include <mie/gmp_util.hpp>
#include <mie/ec.hpp>
#include <mie/ecparam.hpp>
#include <time.h>

#if defined(_WIN64) || defined(__x86_64__)
//	#define USE_MONT_FP
#endif
#ifdef USE_MONT_FP
#include <mie/mont_fp.hpp>
typedef mie::MontFpT<3> Fp_3;
typedef mie::MontFpT<4> Fp_4;
#else
typedef mie::FpT<mie::Gmp> Fp_3;
typedef mie::FpT<mie::Gmp> Fp_4;
typedef mie::FpT<mie::Gmp> Fp_ext;
#endif

struct tagZn;
typedef mie::FpT<mie::Gmp, tagZn> Zn;

template<class Fp>
struct Test {
	typedef mie::EcT<Fp> Ec;
	const mie::EcParam& para;
	Test(const mie::EcParam& para)
		: para(para)
	{
		Fp::setModulo(para.p);
		Zn::setModulo(para.p);
		Ec::setParam(para.a, para.b);
//		CYBOZU_TEST_EQUAL(para.bitLen, Fp(-1).getBitLen());
	}
	void cstr() const
	{
		Ec O;
		CYBOZU_TEST_ASSERT(O.isZero());
		Ec P;
		Ec::neg(P, O);
		CYBOZU_TEST_EQUAL(P, O);
	}
	void ope() const
	{
		Fp x(para.gx);
		Fp y(para.gy);
		Zn n(para.n);
		CYBOZU_TEST_ASSERT(Ec::isValid(x, y));
		Ec P(x, y), Q, R, O;
		{
			Ec::neg(Q, P);
			CYBOZU_TEST_EQUAL(Q.x, P.x);
			CYBOZU_TEST_EQUAL(Q.y, -P.y);

			R = P + Q;
			CYBOZU_TEST_ASSERT(R.isZero());

			R = P + O;
			CYBOZU_TEST_EQUAL(R, P);
			R = O + P;
			CYBOZU_TEST_EQUAL(R, P);
		}

		{
			Ec::dbl(R, P);
			Ec R2 = P + P;
			CYBOZU_TEST_EQUAL(R, R2);
			Ec R3L = R2 + P;
			Ec R3R = P + R2;
			CYBOZU_TEST_EQUAL(R3L, R3R);
			Ec::power(R, P, 2);
			CYBOZU_TEST_EQUAL(R, R2);
			Ec R4L = R3L + R2;
			Ec R4R = R2 + R3L;
			CYBOZU_TEST_EQUAL(R4L, R4R);
			Ec::power(R, P, 5);
			CYBOZU_TEST_EQUAL(R, R4L);
		}
		{
			R = P;
			for (int i = 0; i < 10; i++) {
				R += P;
			}
			Ec R2;
			Ec::power(R2, P, 11);
			CYBOZU_TEST_EQUAL(R, R2);
		}
		Ec::power(R, P, n - 1);
		CYBOZU_TEST_EQUAL(R, -P);
		Ec::power(R, P, n);
		CYBOZU_TEST_ASSERT(R.isZero());
	}

	void power() const
	{
		Fp x(para.gx);
		Fp y(para.gy);
		Ec P(x, y);
		Ec Q;
		Ec R;
		for (int i = 0; i < 100; i++) {
			Ec::power(Q, P, i);
			CYBOZU_TEST_EQUAL(Q, R);
			R += P;
		}
	}

	void neg_power() const
	{
		Fp x(para.gx);
		Fp y(para.gy);
		Ec P(x, y);
		Ec Q;
		Ec R;
		for (int i = 0; i < 100; i++) {
			Ec::power(Q, P, -i);
			CYBOZU_TEST_EQUAL(Q, R);
			R -= P;
		}
	}

	void power_fp() const
	{
		Fp x(para.gx);
		Fp y(para.gy);
		Ec P(x, y);
		Ec Q;
		Ec R;
		for (int i = 0; i < 100; i++) {
			Ec::power(Q, P, Zn(i));
			CYBOZU_TEST_EQUAL(Q, R);
			R += P;
		}
	}

	template<class F>
	void test(F f, const char *msg) const
	{
		const int N = 300000;
		Fp x(para.gx);
		Fp y(para.gy);
		Ec P(x, y);
		Ec Q = P + P + P;
		clock_t begin = clock();
		for (int i = 0; i < N; i++) {
			f(Q, P, Q);
		}
		clock_t end = clock();
		printf("%s %.2fusec\n", msg, (end - begin) / double(CLOCKS_PER_SEC) / N * 1e6);
	}
	/*
		add 8.71usec -> 6.94
		sub 6.80usec -> 4.84
		dbl 9.59usec -> 7.75
		pos 2730usec -> 2153
	*/
	void bench() const
	{
		Fp x(para.gx);
		Fp y(para.gy);
		Ec P(x, y);
		Ec Q = P + P + P;
		CYBOZU_BENCH("add", Ec::add, Q, P, Q);
		CYBOZU_BENCH("sub", Ec::sub, Q, P, Q);
		CYBOZU_BENCH("dbl", Ec::dbl, P, P);
		Zn z("0x9b2f2f6d9c5628a7844163d015be86344082aa88d95e2f9");
		CYBOZU_BENCH("pow", Ec::power, P, P, z);
	}
/*
Affine : sandy-bridge
add 3.17usec
sub 2.43usec
dbl 3.32usec
pow 905.00usec
Jacobi
add 2.34usec
sub 2.65usec
dbl 1.56usec
pow 499.00usec
*/
	void run() const
	{
		puts("cstr");
		cstr();
		puts("ope");
		ope();
		puts("power");
		power();
		puts("neg_power");
		neg_power();
		puts("power_fp");
		power_fp();
#ifdef NDEBUG
		puts("bench");
		bench();
#endif
	}
private:
	Test(const Test&);
	void operator=(const Test&);
};

template<class Fp>
void test_sub(const mie::EcParam *para, size_t paraNum)
{
	for (size_t i = 0; i < paraNum; i++) {
		puts(para[i].name);
		Test<Fp>(para[i]).run();
	}
}

CYBOZU_TEST_AUTO(all)
{
#ifdef USE_MONT_FP
	puts("use MontFp");
#else
	puts("use GMP");
#endif
	const struct mie::EcParam para3[] = {
		mie::ecparam::p160_1,
		mie::ecparam::secp160k1,
		mie::ecparam::secp192k1,
	};
	test_sub<Fp_3>(para3, CYBOZU_NUM_OF_ARRAY(para3));

	const struct mie::EcParam para4[] = {
		mie::ecparam::secp224k1,
		mie::ecparam::secp256k1,
	};
	test_sub<Fp_4>(para4, CYBOZU_NUM_OF_ARRAY(para4));
#ifndef USE_MONT_FP
	const struct mie::EcParam para_ext[] = {
		mie::ecparam::secp384r1,
		mie::ecparam::secp521r1
	};
	test_sub<Fp_ext>(para_ext, CYBOZU_NUM_OF_ARRAY(para_ext));
#endif
}
