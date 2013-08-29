#pragma once
/**
	@file
	@brief Fp generator
	@author MITSUNARI Shigeo(@herumi)
	@license modified new BSD license
	http://opensource.org/licenses/BSD-3-Clause
*/
#include <stdio.h>
#include <assert.h>
#include <cybozu/exception.hpp>
#ifndef XBYAK_NO_OP_NAMES
	#define XBYAK_NO_OP_NAMES
#endif
#include <xbyak/xbyak.h>
#ifdef XBYAK32
	#error "not support 32-bit mode"
#endif
#include <xbyak/xbyak_util.h>


namespace mie {

namespace montgomery {

/*
	get pp such that p * pp = -1 mod M,
	where p is prime and M = 1 << 64(or 32).
	@param pLow [in] p mod M
	T is uint32_t or uint64_t
*/
template<class T>
T getCoff(T pLow)
{
	T ret = 0;
	T t = 0;
	T x = 1;

	for (size_t i = 0; i < sizeof(T) * 8; i++) {
		if ((t & 1) == 0) {
			t += pLow;
			ret += x;
		}
		t >>= 1;
		x <<= 1;
	}
	return ret;
}

} // montgomery

namespace fp_gen_local {

class MemReg {
	const Xbyak::Reg64 *r_;
	const Xbyak::RegExp *m_;
	int offset_;
public:
	MemReg(const Xbyak::Reg64 *r, const Xbyak::RegExp *m, int offset) : r_(r), m_(m), offset_(offset) {}
	bool isReg() const { return r_ != 0; }
	const Xbyak::Reg64& getReg() const { return *r_; }
	Xbyak::RegExp getMem() const { return *m_ + offset_ * (int)sizeof(size_t); }
};

struct MixPack {
	const Xbyak::util::Pack *p;
	int pn;
	const Xbyak::RegExp *m;
	int mn;
	MixPack(const Xbyak::util::Pack *p, const Xbyak::RegExp *m, int mn)
		: p(p), pn(p ? (int)p->size() : 0), m(m), mn(mn)
	{
	}
	int size() const { return pn + mn; }
	bool isReg(int n) const { return n < pn; }
	const Xbyak::Reg64& getReg(int n) const
	{
		assert(n < pn);
		return (*p)[n];
	}
	Xbyak::RegExp getMem(int n) const
	{
		assert(pn <= n && n < size());
		return *m + (n - pn) * (int)sizeof(size_t);
	}
	MemReg operator[](int n) const
	{
		return MemReg((n < pn) ? &(*p)[n] : 0, (n < pn) ? 0 : m, n - pn);
	}
};

} // fp_gen_local

/*
	op(r, rm);
	r  : reg
	rm : Reg/Mem
*/
#define MIE_FP_GEN_OP_RM(op, r, rm) \
if (rm.isReg()) { \
	op(r, rm.getReg()); \
} else { \
	op(r, ptr [rm.getMem()]); \
}

/*
	op(rm, r);
	rm : Reg/Mem
	r  : reg
*/
#define MIE_FP_GEN_OP_MR(op, rm, r) \
if (rm.isReg()) { \
	op(rm.getReg(), r); \
} else { \
	op(ptr [rm.getMem()], r); \
}

struct FpGenerator : Xbyak::CodeGenerator {
	typedef Xbyak::RegExp RegExp;
	typedef Xbyak::Reg64 Reg64;
	typedef Xbyak::Operand Operand;
	typedef Xbyak::util::StackFrame StackFrame;
	typedef Xbyak::util::Pack Pack;
	typedef fp_gen_local::MixPack MixPack;
	typedef fp_gen_local::MemReg MemReg;
	static const int UseRDX = Xbyak::util::UseRDX;
	static const int UseRCX = Xbyak::util::UseRCX;
	Xbyak::util::Cpu cpu_;
	const uint64_t *p_;
	uint64_t pp_;
	int pn_;
	bool isFullBit_;
	// add/sub without carry. return true if overflow
	typedef bool (*bool3op)(uint64_t*, const uint64_t*, const uint64_t*);

	// add/sub with mod
	typedef void (*void3op)(uint64_t*, const uint64_t*, const uint64_t*);

	// mul without carry. return top of z
	typedef uint64_t (*uint3opI)(uint64_t*, const uint64_t*, uint64_t);

	// neg
	typedef void (*void2op)(uint64_t*, const uint64_t*);

	// preInv
	typedef int (*int2op)(uint64_t*, const uint64_t*);
	bool3op addNc_;
	bool3op subNc_;
	void3op add_;
	void3op sub_;
	void3op mul_;
	uint3opI mulI_;
	void2op neg_;
	void2op shr1_;
	int2op preInv_;
	FpGenerator()
		: CodeGenerator(4096 * 4)
		, p_(0)
		, pp_(0)
		, pn_(0)
		, isFullBit_(0)
		, addNc_(0)
		, subNc_(0)
		, add_(0)
		, sub_(0)
		, mul_(0)
		, mulI_(0)
		, neg_(0)
		, shr1_(0)
		, preInv_(0)
	{
	}
	/*
		@param p [in] pointer to prime
		@param pn [in] length of prime
	*/
	void init(const uint64_t *p, size_t pn)
	{
		if (pn < 2) throw cybozu::Exception("mie:FpGenerator:small pn") << pn;
		p_ = p;
		pp_ = montgomery::getCoff(p[0]);
		pn_ = (int)pn;
		isFullBit_ = (p_[pn_ - 1] >> 63) != 0;
//		printf("p=%p, pn_=%d, isFullBit_=%d\n", p_, pn_, isFullBit_);

		setSize(0); // reset code
		align(16);
		addNc_ = getCurr<bool3op>();
		gen_addSubNc(true);
		align(16);
		subNc_ = getCurr<bool3op>();
		gen_addSubNc(false);
		align(16);
		add_ = getCurr<void3op>();
		gen_addMod();
		align(16);
		sub_ = getCurr<void3op>();
		gen_sub();
		align(16);
		neg_ = getCurr<void2op>();
		gen_neg();
		align(16);
		mulI_ = getCurr<uint3opI>();
		gen_mulI();
		align(16);
		mul_ = getCurr<void3op>();
		gen_mul();
		align(16);
		shr1_ = getCurr<void2op>();
		gen_shr1();
		preInv_ = getCurr<int2op>();
		gen_preInv();
	}
	void gen_addSubNc(bool isAdd)
	{
		StackFrame sf(this, 3);
		if (isAdd) {
			gen_raw_add(sf.p[0], sf.p[1], sf.p[2], rax);
		} else {
			gen_raw_sub(sf.p[0], sf.p[1], sf.p[2], rax);
		}
		setc(al);
		movzx(eax, al);
	}
	/*
		pz[] = px[] + py[]
	*/
	void gen_raw_add(const RegExp& pz, const RegExp& px, const RegExp& py, const Reg64& t)
	{
		mov(t, ptr [px]);
		add(t, ptr [py]);
		mov(ptr [pz], t);
		for (int i = 1; i < pn_; i++) {
			mov(t, ptr [px + i * 8]);
			adc(t, ptr [py + i * 8]);
			mov(ptr [pz + i * 8], t);
		}
	}
	/*
		pz[] = px[] - py[]
	*/
	void gen_raw_sub(const RegExp& pz, const RegExp& px, const RegExp& py, const Reg64& t)
	{
		mov(t, ptr [px]);
		sub(t, ptr [py]);
		mov(ptr [pz], t);
		for (int i = 1; i < pn_; i++) {
			mov(t, ptr [px + i * 8]);
			sbb(t, ptr [py + i * 8]);
			mov(ptr [pz + i * 8], t);
		}
	}
	/*
		pz[] = -px[]
	*/
	void gen_raw_neg(const RegExp& pz, const RegExp& px, const Reg64& t0, const Reg64& t1)
	{
		inLocalLabel();
		mov(t0, ptr [px]);
		test(t0, t0);
		jnz(".neg");
		if (pn_ > 1) {
			for (int i = 1; i < pn_; i++) {
				or_(t0, ptr [px + i * 8]);
			}
			jnz(".neg");
		}
		// zero
		for (int i = 0; i < pn_; i++) {
			mov(ptr [pz + i * 8], t0);
		}
		jmp(".exit");
	L(".neg");
		mov(t1, (size_t)p_);
		gen_raw_sub(pz, t1, px, t0);
	L(".exit");
		outLocalLabel();
	}
	/*
		(rdx:pz[]) = px[] * y
		use rax, rdx, pw[]
		@note this is general version(maybe not so fast)
	*/
	void gen_raw_mulI(const RegExp& pz, const RegExp& px, const Reg64& y, const RegExp& pw, const Reg64& t, int n)
	{
		assert(n >= 2);
		if (n == 2) {
			mov(rax, ptr [px]);
			mul(y);
			mov(ptr [pz], rax);
			mov(t, rdx);
			mov(rax, ptr [px + 8]);
			mul(y);
			add(rax, t);
			adc(rdx, 0);
			mov(ptr [pz + 8], rax);
			return;
		}
		for (int i = 0; i < n; i++) {
			mov(rax, ptr [px + i * 8]);
			mul(y);
			mov(ptr [pz + i * 8], rax);
			if (i < n - 1) {
				mov(ptr [pw + i * 8], rdx);
			}
		}
		for (int i = 1; i < n; i++) {
			mov(t, ptr [pz + i * 8]);
			if (i == 1) {
				add(t, ptr [pw + (i - 1) * 8]);
			} else {
				adc(t, ptr [pw + (i - 1) * 8]);
			}
			mov(ptr [pz + i * 8], t);
		}
		adc(rdx, 0);
	}
	/*
		(rdx:pz[]) = px[] * y
		use rax, rdx, pw[]
	*/
	void gen_raw_mulI_with_mulx(const RegExp& pz, const RegExp& px, const Reg64& y, const Reg64& t0, const Reg64& t1, int n)
	{
		mov(rdx, ptr [px]);
		mulx(rax, t0, y);
		mov(ptr [pz], t0);
		const Reg64 *pt0 = &rax;
		const Reg64 *pt1 = &t1;
		for (int i = 1; i < n; i++) {
			mov(rdx, ptr [px + 8 * i]);
			mulx(*pt1, t0, y);
			if (i == 1) {
				add(*pt0, t0);
			} else {
				adc(*pt0, t0);
			}
			mov(ptr [pz + 8 * i], *pt0);
			std::swap(pt0, pt1);
		}
		if (pt1 == &rax) mov(rax, *pt0);
		adc(rax, 0);
	}
	void gen_mulI()
	{
		const bool useMulx = cpu_.has(Xbyak::util::Cpu::tBMI2);
		if (useMulx) {
			printf("use mulx for mulI(%d)\n", pn_);
			// mulx H, L, x ; [H:L] = x * rdx
			StackFrame sf(this, 3, 2 | UseRDX);
			const Reg64& pz = sf.p[0];
			const Reg64& px = sf.p[1];
			const Reg64& y = sf.p[2];
			gen_raw_mulI_with_mulx(pz, px, y, sf.t[0], sf.t[1], pn_);
			return;
		}
		StackFrame sf(this, 3, 1 | UseRDX, pn_ * 8);
		const Reg64& pz = sf.p[0];
		const Reg64& px = sf.p[1];
		const Reg64& y = sf.p[2];
		gen_raw_mulI(pz, px, y, rsp, sf.t[0], pn_);
		mov(rax, rdx);
	}
	/*
		pz[] = px[]
	*/
	void gen_mov(const RegExp& pz, const RegExp& px, const Reg64& t, int n)
	{
		for (int i = 0; i < n; i++) {
			mov(t, ptr [px + i * 8]);
			mov(ptr [pz + i * 8], t);
		}
	}
	void gen_addMod3()
	{
		StackFrame sf(this, 3, 7);
		const Reg64& pz = sf.p[0];
		const Reg64& px = sf.p[1];
		const Reg64& py = sf.p[2];

		const Reg64& t0 = sf.t[0];
		const Reg64& t1 = sf.t[1];
		const Reg64& t2 = sf.t[2];
		const Reg64& t3 = sf.t[3];
		const Reg64& t4 = sf.t[4];
		const Reg64& t5 = sf.t[5];
		const Reg64& t6 = sf.t[6];

		xor_(t6, t6);
		load_rm(Pack(t2, t1, t0), px);
		add_rm(Pack(t2, t1, t0), py);
		mov_rr(Pack(t5, t4, t3), Pack(t2, t1, t0));
		adc(t6, 0);
		mov(rax, (size_t)p_);
		sub_rm(Pack(t5, t4, t3), rax);
		sbb(t6, 0);
		cmovc(t5, t2);
		cmovc(t4, t1);
		cmovc(t3, t0);
		store_mr(pz, Pack(t5, t4, t3));
	}
	void gen_subMod_le4(int n)
	{
		assert(2 <= n && n <= 4);
		StackFrame sf(this, 3, (n - 1) * 2);
		const Reg64& pz = sf.p[0];
		const Reg64& px = sf.p[1];
		const Reg64& py = sf.p[2];

		Pack rx = sf.t.sub(0, n - 1);
		rx.append(px); // rx = [px, t1, t0]
		Pack ry = sf.t.sub(n - 1, n - 1);
		ry.append(rax); // ry = [rax, t3, t2]

		load_rm(rx, px); // destroy px
		sub_rm(rx, py);
#if 0
		sbb(ry[0], ry[0]); // rx[0] = (x > y) ? 0 : -1
		for (int i = 1; i < n; i++) mov(ry[i], ry[0]);
		mov(py, (size_t)p_);
		for (int i = 0; i < n; i++) and_(ry[i], qword [py + 8 * i]);
		add_rr(rx, ry);
#else
		// a little faster
		sbb(py, py); // py = (x > y) ? 0 : -1
		mov(rax, (size_t)p_);
		load_rm(ry, rax); // destroy rax
		for (size_t i = 0; i < ry.size(); i++) {
			and_(ry[i], py);
		}
		add_rr(rx, ry);
#endif
		store_mr(pz, rx);
	}
	void gen_addMod()
	{
		if (pn_ == 3) {
			gen_addMod3();
			return;
		}
		StackFrame sf(this, 3, 0, pn_ * 8);
		const Reg64& pz = sf.p[0];
		const Reg64& px = sf.p[1];
		const Reg64& py = sf.p[2];
		const Xbyak::CodeGenerator::LabelType jmpMode = pn_ < 5 ? T_AUTO : T_NEAR;

		inLocalLabel();
		gen_raw_add(pz, px, py, rax);
		mov(px, (size_t)p_); // destroy px
		if (isFullBit_) {
			jc(".over", jmpMode);
		}
		gen_raw_sub(rsp, pz, px, rax);
		jc(".exit", jmpMode);
		gen_mov(pz, rsp, rax, pn_);
		if (isFullBit_) {
			jmp(".exit", jmpMode);
			L(".over");
			gen_raw_sub(pz, pz, px, rax);
		}
		L(".exit");
		outLocalLabel();
	}
	void gen_sub()
	{
		if (pn_ <= 4) {
			gen_subMod_le4(pn_);
			return;
		}
		StackFrame sf(this, 3);
		const Reg64& pz = sf.p[0];
		const Reg64& px = sf.p[1];
		const Reg64& py = sf.p[2];
		const Xbyak::CodeGenerator::LabelType jmpMode = pn_ < 5 ? T_AUTO : T_NEAR;

		inLocalLabel();
		gen_raw_sub(pz, px, py, rax);
		jnc(".exit", jmpMode);
		mov(px, (size_t)p_);
		gen_raw_add(pz, pz, px, rax);
	L(".exit");
		outLocalLabel();
	}
	void gen_neg()
	{
		StackFrame sf(this, 2, 2);
		const Reg64& pz = sf.p[0];
		const Reg64& px = sf.p[1];
		gen_raw_neg(pz, px, sf.t[0], sf.t[1]);
	}
	void gen_shr1()
	{
		const int c = 1;
		StackFrame sf(this, 2, 1);
		const Reg64 *t0 = &rax;
		const Reg64 *t1 = &sf.t[0];
		const Reg64& pz = sf.p[0];
		const Reg64& px = sf.p[1];
		mov(*t0, ptr [px]);
		for (int i = 0; i < pn_ - 1; i++) {
			mov(*t1, ptr [px + 8 * (i + 1)]);
			shrd(*t0, *t1, c);
			mov(ptr [pz + i * 8], *t0);
			std::swap(t0, t1);
		}
		shr(*t0, c);
		mov(ptr [pz + (pn_ - 1) * 8], *t0);
	}
	void gen_mul()
	{
		if (pn_ == 3) {
			gen_montMul3(p_, pp_);
		} else if (pn_ == 4) {
			gen_montMul4(p_, pp_);
		} else if (pn_ <= 9) {
			gen_montMulN(p_, pp_, pn_);
		} else {
			throw cybozu::Exception("mie:FpGenerator:gen_mul:not implemented for") << pn_;
		}
	}
	/*
		input (pz[], px[], py[])
		z[] <- montgomery(x[], y[])
	*/
	void gen_montMulN(const uint64_t *p, uint64_t pp, int n)
	{
		StackFrame sf(this, 3, 3 | UseRDX, (n * 3 + 2) * 8);
		const Reg64& pz = sf.p[0];
		const Reg64& px = sf.p[1];
		const Reg64& py = sf.p[2];
		const Reg64& t = sf.t[0];
		const Reg64& y = sf.t[1];
		const Reg64& pAddr = sf.t[2];
		const RegExp pw1 = rsp; // pw1[0..n-1]
		const RegExp pw2 = pw1 + n * 8; // pw2[0..n-1]
		const RegExp pc = pw2 + n * 8; // pc[0..n+1]
		mov(pAddr, (size_t)p);

		for (int i = 0; i < n; i++) {
			mov(y, ptr [py + i * 8]);
			montgomeryN_1(pp, n, pc, px, y, pAddr, t, pw1, pw2, i == 0);
		}
		// pz[] = pc[] - p[]
		gen_raw_sub(pz, pc, pAddr, t);
		if (isFullBit_) sbb(qword[pc + n * 8], 0);
		jnc("@f");
		for (int i = 0; i < n; i++) {
			mov(t, ptr [pc + i * 8]);
			mov(ptr [pz + i * 8], t);
		}
	L("@@");
	}
	/*
		input (z, x, y) = (p0, p1, p2)
		z[0..3] <- montgomery(x[0..3], y[0..3])
		destroy gt0, ..., gt9, xm0, xm1, p2
	*/
	void gen_montMul4(const uint64_t *p, uint64_t pp)
	{
		StackFrame sf(this, 3, 10 | UseRDX);
		const Reg64& p0 = sf.p[0];
		const Reg64& p1 = sf.p[1];
		const Reg64& p2 = sf.p[2];

		const Reg64& t0 = sf.t[0];
		const Reg64& t1 = sf.t[1];
		const Reg64& t2 = sf.t[2];
		const Reg64& t3 = sf.t[3];
		const Reg64& t4 = sf.t[4];
		const Reg64& t5 = sf.t[5];
		const Reg64& t6 = sf.t[6];
		const Reg64& t7 = sf.t[7];
		const Reg64& t8 = sf.t[8];
		const Reg64& t9 = sf.t[9];

		movq(xm0, p0); // save p0
		mov(p0, (uint64_t)p);
		movq(xm1, p2);
		mov(p2, ptr [p2]);
		montgomery4_1(pp, t0, t7, t3, t2, t1, p1, p2, p0, t4, t5, t6, t8, t9, true);

		movq(p2, xm1);
		mov(p2, ptr [p2 + 8]);
		montgomery4_1(pp, t1, t0, t7, t3, t2, p1, p2, p0, t4, t5, t6, t8, t9, false);

		movq(p2, xm1);
		mov(p2, ptr [p2 + 16]);
		montgomery4_1(pp, t2, t1, t0, t7, t3, p1, p2, p0, t4, t5, t6, t8, t9, false);

		movq(p2, xm1);
		mov(p2, ptr [p2 + 24]);
		montgomery4_1(pp, t3, t2, t1, t0, t7, p1, p2, p0, t4, t5, t6, t8, t9, false);
		// [t7:t3:t2:t1:t0]

		mov(t4, t0);
		mov(t5, t1);
		mov(t6, t2);
		mov(rdx, t3);
		sub_rm(Pack(t3, t2, t1, t0), p0);
		if (isFullBit_) sbb(t7, 0);
		cmovc(t0, t4);
		cmovc(t1, t5);
		cmovc(t2, t6);
		cmovc(t3, rdx);

		movq(p0, xm0); // load p0
		store_mr(p0, Pack(t3, t2, t1, t0));
	}
	/*
		input (z, x, y) = (p0, p1, p2)
		z[0..2] <- montgomery(x[0..2], y[0..2])
		destroy gt0, ..., gt9, xm0, xm1, p2
	*/
	void gen_montMul3(const uint64_t *p, uint64_t pp)
	{
		StackFrame sf(this, 3, 10 | UseRDX);
		const Reg64& p0 = sf.p[0];
		const Reg64& p1 = sf.p[1];
		const Reg64& p2 = sf.p[2];

		const Reg64& t0 = sf.t[0];
		const Reg64& t1 = sf.t[1];
		const Reg64& t2 = sf.t[2];
		const Reg64& t3 = sf.t[3];
		const Reg64& t4 = sf.t[4];
		const Reg64& t5 = sf.t[5];
		const Reg64& t6 = sf.t[6];
		const Reg64& t7 = sf.t[7];
		const Reg64& t8 = sf.t[8];
		const Reg64& t9 = sf.t[9];

		movq(xm0, p0); // save p0
		mov(t7, (uint64_t)p);
		mov(t9, ptr [p2]);
		//                c3, c2, c1, c0, px, y,  p,
		montgomery3_1(pp, t0, t3, t2, t1, p1, t9, t7, t4, t5, t6, t8, p0, true);
		mov(t9, ptr [p2 + 8]);
		montgomery3_1(pp, t1, t0, t3, t2, p1, t9, t7, t4, t5, t6, t8, p0, false);

		mov(t9, ptr [p2 + 16]);
		montgomery3_1(pp, t2, t1, t0, t3, p1, t9, t7, t4, t5, t6, t8, p0, false);

		// [t3:t2:t1:t0]
		mov(t4, t0);
		mov(t5, t1);
		mov(t6, t2);
		sub_rm(Pack(t2, t1, t0), t7);
		if (isFullBit_) sbb(t3, 0);
		cmovc(t0, t4);
		cmovc(t1, t5);
		cmovc(t2, t6);
		movq(p0, xm0);
		store_mr(p0, Pack(t2, t1, t0));
	}
	static inline void debug_put_inner(const uint64_t *ptr, int n)
	{
		printf("debug ");
		for (int i = 0; i < n; i++) {
			printf("%016llx", (long long)ptr[n - 1 - i]);
		}
		printf("\n");
	}
#ifdef _MSC_VER
	void debug_put(const RegExp& m, int n)
	{
		assert(n <= 8);
		static uint64_t regBuf[7];

		push(rax);
		mov(rax, (size_t)regBuf);
		mov(ptr [rax + 8 * 0], rcx);
		mov(ptr [rax + 8 * 1], rdx);
		mov(ptr [rax + 8 * 2], r8);
		mov(ptr [rax + 8 * 3], r9);
		mov(ptr [rax + 8 * 4], r10);
		mov(ptr [rax + 8 * 5], r11);
		mov(rcx, ptr [rsp + 8]); // org rax
		mov(ptr [rax + 8 * 6], rcx); // save
		mov(rcx, ptr [rax + 8 * 0]); // org rcx
		pop(rax);

		lea(rcx, ptr [m]);
		mov(rdx, n);
		mov(rax, (size_t)debug_put_inner);
		sub(rsp, 32);
		call(rax);
		add(rsp, 32);

		push(rax);
		mov(rax, (size_t)regBuf);
		mov(rcx, ptr [rax + 8 * 0]);
		mov(rdx, ptr [rax + 8 * 1]);
		mov(r8, ptr [rax + 8 * 2]);
		mov(r9, ptr [rax + 8 * 3]);
		mov(r10, ptr [rax + 8 * 4]);
		mov(r11, ptr [rax + 8 * 5]);
		mov(rax, ptr [rax + 8 * 6]);
		add(rsp, 8);
	}
#endif
	/*
		z >>= c
	*/
	void shr_mp(const MixPack& z, uint8_t c, const Reg64& t)
	{
		const int n = z.size();
		for (int i = 0; i < n - 1; i++) {
			if (z.isReg(i + 1)) {
				shrd(z.getReg(i), z.getReg(i + 1), c);
			} else if (z.isReg(i)) {
				mov(t, ptr [z.getMem(i + 1)]);
				shrd(z.getReg(i), t, c);
			} else {
				mov(t, ptr [z.getMem(i + 1)]);
				shrd(ptr [z.getMem(i)], t, c);
			}
		}
		if (z.isReg(n - 1)) {
			shr(z.getReg(n - 1), c);
		} else {
			shr(qword [z.getMem(n - 1)], c);
		}
	}
	/*
		z *= 2
	*/
	void twice_mp(const MixPack& z, const Reg64& t)
	{
		g_add(z[0], z[0], t);
		const int n = z.size();
		for (int i = 1; i < n; i++) {
			g_adc(z[i], z[i], t);
		}
	}
	/*
		z += x
	*/
	void add_mp(const MixPack& z, const MixPack& x, const Reg64& t)
	{
		assert(z.size() == x.size());
		g_add(z[0], x[0], t);
		for (int i = 1, n = z.size(); i < n; i++) {
			g_adc(z[i], x[i], t);
		}
	}
	/*
		z -= x
	*/
	void sub_mp(const MixPack& z, const MixPack& x, const Reg64& t)
	{
		assert(z.size() == x.size());
		g_sub(z[0], x[0], t);
		for (int i = 1, n = z.size(); i < n; i++) {
			g_sbb(z[i], x[i], t);
		}
	}
	void store_mp(const RegExp& m, const MixPack& z, const Reg64& t)
	{
		for (int i = 0, n = z.size(); i < n; i++) {
			if (z.isReg(i)) {
				mov(ptr [m + i * 8], z.getReg(i));
			} else {
				mov(t, ptr [z.getMem(i)]);
				mov(ptr [m + i * 8], t);
			}
		}
	}
	void load_mp(const MixPack& z, const RegExp& m, const Reg64& t)
	{
		for (int i = 0, n = z.size(); i < n; i++) {
			if (z.isReg(i)) {
				mov(z.getReg(i), ptr [m + i * 8]);
			} else {
				mov(t, ptr [m + i * 8]);
				mov(ptr [z.getMem(i)], t);
			}
		}
	}
	void set_mp(const MixPack& z, const Reg64& t)
	{
		for (int i = 0, n = z.size(); i < n; i++) {
			MIE_FP_GEN_OP_MR(mov, z[i], t)
		}
	}
	void mov_mp(const MixPack& z, const MixPack& x, const Reg64& t)
	{
		for (int i = 0, n = z.size(); i < n; i++) {
			const MemReg zi = z[i], xi = x[i];
			if (z.isReg(i)) {
				MIE_FP_GEN_OP_RM(mov, zi.getReg(), xi)
			} else {
				if (x.isReg(i)) {
					mov(ptr [z.getMem(i)], x.getReg(i));
				} else {
					mov(t, ptr [x.getMem(i)]);
					mov(ptr [z.getMem(i)], t);
				}
			}
		}
	}
	void debug_put_mp(const MixPack& mp, int n, const Reg64& t)
	{
		if (n >= 10) exit(1);
		static uint64_t buf[10];
		movq(xm0, rax);
		mov(rax, (size_t)buf);
		store_mp(rax, mp, t);
		movq(rax, xm0);
		push(rax);
		mov(rax, (size_t)buf);
		debug_put(rax, n);
		pop(rax);
	}

	/*
		input (r, x) = (p0, p1)
		int k = preInvC(r, x)
	*/
	void gen_preInv()
	{
		assert(pn_ >= 2);
		const int freeRegNum = 13;
		if (pn_ > 9) {
			throw cybozu::Exception("mie:FpGenerator:gen_preInv:large pn_") << pn_;
		}
		StackFrame sf(this, 2, 10 | UseRDX | UseRCX, (std::max(0, pn_ * 5 - freeRegNum) + 1 + (isFullBit_ ? 1 : 0)) * 8);
		const Reg64& pr = sf.p[0];
		const Reg64& px = sf.p[1];
		const Reg64& t = rcx;
		/*
			rax, : k, rcx : temporary
			use rdx, pr, px in main loop, so we can use 13 registers
			v = t[0, pn_) : all registers
		*/
		int rspPos = 0;

		assert((int)sf.t.size() >= pn_);
		const Pack v = sf.t.sub(0, pn_);
		const MixPack vv(&v, 0, 0);

		Pack remain = sf.t.sub(pn_);
		if (pn_ > 2) {
			remain.append(rdx).append(pr).append(px);
		}
		const int uRegNum = std::min((int)remain.size(), pn_);
		const int uMemNum = pn_ - uRegNum;
		const Pack u = remain.sub(0, uRegNum);
		const RegExp uMem = rsp + rspPos;
		const MixPack uu(&u, &uMem, uMemNum);
		rspPos += uMemNum * 8;
		remain = remain.sub(uRegNum);

		const int rRegNum = std::min((int)remain.size(), pn_);
		const int rMemNum = pn_ - rRegNum;
		const Pack r = remain.sub(0, rRegNum);
		const RegExp rMem = rsp + rspPos;
		const MixPack rr(&r, &rMem, rMemNum);
		rspPos += rMemNum * 8;
		remain = remain.sub(rRegNum);

		const int sRegNum = std::min((int)remain.size(), pn_);
		const int sMemNum = pn_ - sRegNum;
		const Pack s = remain.sub(0, sRegNum);
		const RegExp sMem = rsp + rspPos;
		const MixPack ss(&s, &sMem, sMemNum);
		rspPos += sMemNum * 8;
		remain = remain.sub(sRegNum);

		const int keepRegNum = std::min((int)remain.size(), pn_);
		const int keepMemNum = pn_ - keepRegNum;
		const Pack keep = remain.sub(0, keepRegNum);
		const RegExp keepMem = rsp + rspPos;
		const MixPack keep_v(&keep, &keepMem, keepMemNum);
		rspPos += keepMemNum * 8;
		remain = remain.sub(keepRegNum);

		const RegExp keep_pr = rsp + rspPos;
		rspPos += 8;
		const RegExp rTop = rsp + rspPos; // used if isFullBit_

		inLocalLabel();
		mov(ptr [keep_pr], pr);
		load_rm(v, px); // v = x
		// px is free frome here
		mov(rax, (size_t)p_);
		load_mp(uu, rax, t); // u = p_
		// k = 0
		xor_(rax, rax);
		// rTop = 0
		if (isFullBit_) {
			mov(ptr [rTop], rax);
		}
		// r = 0;
		set_mp(rr, rax);
		// s = 1
		set_mp(ss, rax);
		if (ss.isReg(0)) {
			mov(ss.getReg(0), 1);
		} else {
			mov(qword [ss.getMem(0)], 1);
		}
		jmp(".lp");
		align(16);
	L(".lp");
		or_r(t, v);
		jz(".exit", T_NEAR);

		if (uu.isReg(0)) {
			test(uu.getReg(0), 1);
		} else {
			test(qword [uu.getMem(0)], 1);
		}
		jz(".u_even", T_NEAR);
		test(v[0], 1);
		jz(".v_even", T_NEAR);
		mov_mp(keep_v, vv, t);
		sub_mp(vv, uu, t);
		jc(".v_lt_u", T_NEAR);
		add_mp(ss, rr, t);
	L(".v_even");
		shr_mp(vv, 1, t);
		twice_mp(rr, t);
		if (isFullBit_) {
			sbb(t, t);
			mov(ptr [rTop], t);
		}
		inc(rax);
		jmp(".lp", T_NEAR);
	L(".v_lt_u");
		mov_mp(vv, keep_v, t);
		sub_mp(uu, vv, t);
		add_mp(rr, ss, t);
		if (isFullBit_) {
			sbb(t, t);
			mov(ptr [rTop], t);
		}
	L(".u_even");
		shr_mp(uu, 1, t);
		twice_mp(ss, t);
		inc(rax);
		jmp(".lp", T_NEAR);
	L(".exit");
		mov(t, (size_t)p_);
		load_rm(v, t); // v = p
		if (isFullBit_) {
			mov(t, ptr [rTop]);
			test(t, t);
			jz("@f");
			sub_mp(rr, vv, t);
		L("@@");
		}
		sub_mp(vv, rr, t);
		jnc("@f");
		mov(t, (size_t)p_);
		add_rm(v, t);
	L("@@");
		mov(rcx, ptr [keep_pr]);
		store_mr(rcx, v);
		outLocalLabel();
	}
private:
	FpGenerator(const FpGenerator&);
	void operator=(const FpGenerator&);
	void make_op_rm(void (Xbyak::CodeGenerator::*op)(const Xbyak::Operand&, const Xbyak::Operand&), const Reg64& op1, const MemReg& op2)
	{
		if (op2.isReg()) {
			(this->*op)(op1, op2.getReg());
		} else {
			(this->*op)(op1, ptr [op2.getMem()]);
		}
	}
	void make_op_mr(void (Xbyak::CodeGenerator::*op)(const Xbyak::Operand&, const Xbyak::Operand&), const MemReg& op1, const Reg64& op2)
	{
		if (op1.isReg()) {
			(this->*op)(op1.getReg(), op2);
		} else {
			(this->*op)(ptr [op1.getMem()], op2);
		}
	}
	void make_op(void (Xbyak::CodeGenerator::*op)(const Xbyak::Operand&, const Xbyak::Operand&), const MemReg& op1, const MemReg& op2, const Reg64& t)
	{
		if (op1.isReg()) {
			make_op_rm(op, op1.getReg(), op2);
		} else if (op2.isReg()) {
			(this->*op)(ptr [op1.getMem()], op2.getReg());
		} else {
			mov(t, ptr [op2.getMem()]);
			(this->*op)(ptr [op1.getMem()], t);
		}
	}
	void g_add(const MemReg& op1, const MemReg& op2, const Reg64& t) { make_op(&Xbyak::CodeGenerator::add, op1, op2, t); }
	void g_adc(const MemReg& op1, const MemReg& op2, const Reg64& t) { make_op(&Xbyak::CodeGenerator::adc, op1, op2, t); }
	void g_sub(const MemReg& op1, const MemReg& op2, const Reg64& t) { make_op(&Xbyak::CodeGenerator::sub, op1, op2, t); }
	void g_sbb(const MemReg& op1, const MemReg& op2, const Reg64& t) { make_op(&Xbyak::CodeGenerator::sbb, op1, op2, t); }
	/*
		z[] = x[]
	*/
	void mov_rr(const Pack& z, const Pack& x)
	{
		assert(z.size() == x.size());
		for (int i = 0, n = (int)x.size(); i < n; i++) {
			mov(z[i], x[i]);
		}
	}
	/*
		m[] = x[]
	*/
	void store_mr(const RegExp& m, const Pack& x)
	{
		for (int i = 0, n = (int)x.size(); i < n; i++) {
			mov(ptr [m + 8 * i], x[i]);
		}
	}
	/*
		x[] = m[]
	*/
	void load_rm(const Pack& z, const RegExp& m)
	{
		for (int i = 0, n = (int)z.size(); i < n; i++) {
			mov(z[i], ptr [m + 8 * i]);
		}
	}
	/*
		z[] += x[]
	*/
	void add_rr(const Pack& z, const Pack& x)
	{
		add(z[0], x[0]);
		assert(z.size() == x.size());
		for (size_t i = 1, n = z.size(); i < n; i++) {
			adc(z[i], x[i]);
		}
	}
	/*
		z[] += m[]
	*/
	void add_rm(const Pack& z, const RegExp& m)
	{
		add(z[0], ptr [m + 8 * 0]);
		for (int i = 1, n = (int)z.size(); i < n; i++) {
			adc(z[i], ptr [m + 8 * i]);
		}
	}
	/*
		z[] -= m[]
	*/
	void sub_rm(const Pack& z, const RegExp& m)
	{
		sub(z[0], ptr [m + 8 * 0]);
		for (int i = 1, n = (int)z.size(); i < n; i++) {
			sbb(z[i], ptr [m + 8 * i]);
		}
	}
	/*
		t = all or x[i]
		ZF = x is zero
	*/
	void or_r(const Reg64& t, const Pack& x)
	{
		const int n = (int)x.size();
		if (n == 1) {
			test(x[0], x[0]);
		} else {
			mov(t, x[0]);
			for (int i = 1; i < n; i++) {
				or_(t, x[i]);
			}
		}
	}
	/*
		[rdx:x:t1:t0] <- py[2:1:0] * x
		destroy x, t
	*/
	void mul3x1(const RegExp& py, const Reg64& x, const Reg64& t2, const Reg64& t1, const Reg64& t0, const Reg64& t)
	{
		mov(rax, ptr [py]);
		mul(x);
		mov(t0, rax);
		mov(t1, rdx);
		mov(rax, ptr [py + 8]);
		mul(x);
		mov(t, rax);
		mov(t2, rdx);
		mov(rax, ptr [py + 8 * 2]);
		mul(x);
		/*
			rdx:rax
			     t2:t
			        t1:t0
		*/
		add(t1, t);
		adc(rax, t2);
		adc(rdx, 0);
		mov(x, rax);
	}

	/*
		c = [c3:c2:c1:c0]
		c += x[2..0] * y
		q = uint64_t(c0 * pp)
		c = (c + q * p) >> 64
		input  [c3:c2:c1:c0], px, y, p
		output [c0:c3:c2:c1] ; c0 == 0 unless isFullBit_

		@note use rax, rdx, destroy y
	*/
	void montgomery3_1(uint64_t pp, const Reg64& c3, const Reg64& c2, const Reg64& c1, const Reg64& c0,
		const Reg64& px, const Reg64& y, const Reg64& p,
		const Reg64& t0, const Reg64& t1, const Reg64& t2, const Reg64& t3, const Reg64& t4, bool isFirst)
	{
		if (isFirst) {
			mul3x1(px, y, c2, c1, c0, c3);
			mov(c3, rdx);
			// [c3:y:c1:c0] = px[2..0] * y
		} else {
			mul3x1(px, y, t2, t1, t0, t3);
			// [rdx:y:t1:t0] = px[2..0] * y
			if (isFullBit_) xor_(t4, t4);
			add_rr(Pack(c3, y, c1, c0), Pack(rdx, c2, t1, t0));
			if (isFullBit_) adc(t4, 0);
		}
		// [t4:c3:y:c1:c0]
		// t4 = 0 or 1 if isFullBit_, = 0 otherwise
		mov(rax, pp);
		mul(c0); // q = rax
		mov(c2, rax);
		mul3x1(p, c2, t2, t1, t0, t3);
		// [rdx:c2:t1:t0] = p * q
		add(c0, t0); // always c0 is zero
		adc(c1, t1);
		adc(c2, y);
		adc(c3, rdx);
		if (isFullBit_) {
			if (isFirst) {
				adc(c0, 0);
			} else {
				adc(c0, t4);
			}
		}
	}
	/*
		pc[0..n] += x[0..n-1] * y ; pc[] = 0 if isFirst
		pc[n + 1] is temporary used if isFullBit_
		q = uint64_t(pc[0] * pp)
		pc[] = (pc[] + q * p) >> 64
		input : pc[], px[], y, p[], pw1[], pw2[]
		output : pc[0..n]   ; if isFullBit_
		         pc[0..n-1] ; if !isFullBit_
		destroy y
	*/
	void montgomeryN_1(uint64_t pp, int n, const RegExp& pc, const RegExp& px, const Reg64& y, const Reg64& p, const Reg64& t, const RegExp& pw1, const RegExp& pw2, bool isFirst)
	{
		// pc[] += x[] * y
		if (isFirst) {
			gen_raw_mulI(pc, px, y, pw1, t, n);
			mov(ptr [pc + n * 8], rdx);
		} else {
			gen_raw_mulI(pw2, px, y, pw1, t, n);
			mov(t, ptr [pw2 + 0 * 8]);
			add(ptr [pc + 0 * 8], t);
			for (int i = 1; i < n; i++) {
				mov(t, ptr [pw2 + i * 8]);
				adc(ptr [pc + i * 8], t);
			}
			adc(ptr [pc + n * 8], rdx);
			if (isFullBit_) {
				mov(t, 0);
				adc(t, 0);
				mov(qword [pc + (n + 1) * 8], t);
			}
		}
		mov(rax, pp);
		mul(qword [pc]);
		mov(y, rax); // y = q
		gen_raw_mulI(pw2, p, y, pw1, t, n);
		// c[] = (c[] + pw2[]) >> 64
		mov(t, ptr [pw2 + 0 * 8]);
		add(t, ptr [pc + 0 * 8]);
		for (int i = 1; i < n; i++) {
			mov(t, ptr [pw2 + i * 8]);
			adc(t, ptr [pc + i * 8]);
			mov(ptr [pc + (i - 1) * 8], t);
		}
		adc(rdx, ptr [pc + n * 8]);
		mov(ptr [pc + (n - 1) * 8], rdx);
		if (isFullBit_) {
			if (isFirst) {
				mov(t, 0);
			} else {
				mov(t, ptr [pc + (n + 1) * 8]);
			}
			adc(t, 0);
			mov(qword [pc + n * 8], t);
		} else {
			xor_(eax, eax);
			mov(ptr [pc + n * 8], rax);
		}
	}
	/*
		[rdx:x:t2:t1:t0] <- py[3:2:1:0] * x
		destroy x, t
	*/
	void mul4x1(const RegExp& py, const Reg64& x, const Reg64& t3, const Reg64& t2, const Reg64& t1, const Reg64& t0, const Reg64& t)
	{
		mov(rax, ptr [py]);
		mul(x);
		mov(t0, rax);
		mov(t1, rdx);
		mov(rax, ptr [py + 8]);
		mul(x);
		mov(t, rax);
		mov(t2, rdx);
		mov(rax, ptr [py + 8 * 2]);
		mul(x);
		mov(t3, rax);
		mov(rax, x);
		mov(x, rdx);
		mul(qword [py + 8 * 3]);
		add(t1, t);
		adc(t2, t3);
		adc(x, rax);
		adc(rdx, 0);
	}

	/*
		c = [c4:c3:c2:c1:c0]
		c += x[3..0] * y
		q = uint64_t(c0 * pp)
		c = (c + q * p) >> 64
		input  [c4:c3:c2:c1:c0], px, y, p
		output [c0:c4:c3:c2:c1]

		@note use rax, rdx, destroy y
	*/
	void montgomery4_1(uint64_t pp, const Reg64& c4, const Reg64& c3, const Reg64& c2, const Reg64& c1, const Reg64& c0,
		const Reg64& px, const Reg64& y, const Reg64& p,
		const Reg64& t0, const Reg64& t1, const Reg64& t2, const Reg64& t3, const Reg64& t4, bool isFirst)
	{
		if (isFirst) {
			mul4x1(px, y, c3, c2, c1, c0, c4);
			mov(c4, rdx);
			// [c4:y:c2:c1:c0] = px[3..0] * y
		} else {
			mul4x1(px, y, t3, t2, t1, t0, t4);
			// [rdx:y:t2:t1:t0] = px[3..0] * y
			if (isFullBit_) {
				push(px);
				xor_(px, px);
			}
			add_rr(Pack(c4, y, c2, c1, c0), Pack(rdx, c3, t2, t1, t0));
			if (isFullBit_) {
				adc(px, 0);
			}
		}
		// [px:c4:y:c2:c1:c0]
		// px = 0 or 1 if isFullBit_, = 0 otherwise
		mov(rax, pp);
		mul(c0); // q = rax
		mov(c3, rax);
		mul4x1(p, c3, t3, t2, t1, t0, t4);
		add(c0, t0); // always c0 is zero
		adc(c1, t1);
		adc(c2, t2);
		adc(c3, y);
		adc(c4, rdx);
		if (isFullBit_) {
			if (isFirst) {
				adc(c0, 0);
			} else {
				adc(c0, px);
				pop(px);
			}
		}
	}
};

} // mie
