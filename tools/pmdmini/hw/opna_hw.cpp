//=============================================================================
//	opna_hw.cpp - FM::OPNA / FM::OPNAW implementation that drives the real
//	PC-9801-86 (YM2608) instead of the ymfm software emulator.
//
//	Built only when PMDMINI_HW_BACKEND is defined (see opna.h / opnaw.h).
//	Everything synthesis-related (Mix/Count/GetNextEvent/volume trim) is a
//	no-op: the chip runs in real time and is clocked by its own Timer-B.
//
//	SPDX-License-Identifier: GPL-2.0-or-later
//=============================================================================

#include "opna_hw.h"

extern "C" {
#include "opna_hw_c.h"
}

namespace FM {

//-----------------------------------------------------------------------------
//	OPNA
//-----------------------------------------------------------------------------
OPNA::OPNA(IFILEIO *fio)
{
	pfileio = fio;
	if (pfileio)
		pfileio->AddRef();
}

OPNA::~OPNA()
{
	if (pfileio)
		pfileio->Release();
}

void OPNA::setfileio(IFILEIO *fio)
{
	if (fio)
		fio->AddRef();
	if (pfileio)
		pfileio->Release();
	pfileio = fio;
}

bool OPNA::Init(uint32_t c, uint32_t r, bool ip, const TCHAR *path)
{
	(void)c; (void)r; (void)ip; (void)path;
	/* The rhythm PCM lives in the YM2608 mask ROM; nothing to load. */
	return true;
}

bool OPNA::SetRate(uint32_t r)
{
	(void)r;
	return true;
}

bool OPNA::SetRate(uint32_t c, uint32_t r, bool)
{
	(void)c; (void)r;
	return true;
}

bool OPNA::LoadRhythmSample(const TCHAR *)
{
	return true;
}

void OPNA::Reset()
{
	opna_hw_reset();
}

void OPNA::SetVolumeFM(int db)      { (void)db; }
void OPNA::SetVolumePSG(int db)     { (void)db; }
void OPNA::SetVolumeADPCM(int db)   { (void)db; }
void OPNA::SetVolumeRhythmTotal(int db) { (void)db; }
void OPNA::SetVolumeRhythm(int index, int db) { (void)index; (void)db; }

void OPNA::SetReg(uint32_t addr, uint32_t data)
{
	opna_hw_setreg(addr, data);
}

uint32_t OPNA::GetReg(uint32_t addr)
{
	return opna_hw_getreg(addr);
}

uint32_t OPNA::ReadStatus()
{
	return opna_hw_readstatus() & 0x03u;	/* timer-A/B flags only */
}

uint32_t OPNA::ReadStatusEx()
{
	return opna_hw_readstatus() & 0x03u;
}

bool OPNA::Count(uint32_t us)
{
	(void)us;
	return false;
}

uint32_t OPNA::GetNextEvent()
{
	/* Never used outside the offline render loop; report "no event". */
	return 0xffffffffu;
}

void OPNA::Mix(Sample *buffer, int nsamples)
{
	(void)buffer; (void)nsamples;
}

//-----------------------------------------------------------------------------
//	OPNAW
//-----------------------------------------------------------------------------
OPNAW::OPNAW(IFILEIO *fio)
	: OPNA(fio)
{
	fmwait = 0;
	ssgwait = 0;
	rhythmwait = 0;
	adpcmwait = 0;
}

OPNAW::~OPNAW()
{
}

void OPNAW::setfileio(IFILEIO *fio)
{
	OPNA::setfileio(fio);
}

bool OPNAW::Init(uint32_t c, uint32_t r, bool ipflag, const TCHAR *path)
{
	return OPNA::Init(c, r, ipflag, path);
}

bool OPNAW::SetRate(uint32_t c, uint32_t r, bool ipflag)
{
	(void)ipflag;
	return OPNA::SetRate(c, r);
}

void OPNAW::SetFMWait(int32_t nsec)    { fmwait = nsec; }
void OPNAW::SetSSGWait(int32_t nsec)   { ssgwait = nsec; }
void OPNAW::SetRhythmWait(int32_t nsec){ rhythmwait = nsec; }
void OPNAW::SetADPCMWait(int32_t nsec) { adpcmwait = nsec; }

int32_t OPNAW::GetFMWait(void)     { return fmwait; }
int32_t OPNAW::GetSSGWait(void)    { return ssgwait; }
int32_t OPNAW::GetRhythmWait(void) { return rhythmwait; }
int32_t OPNAW::GetADPCMWait(void)  { return adpcmwait; }

void OPNAW::SetReg(uint32_t addr, uint32_t data)
{
	opna_hw_setreg(addr, data);
}

void OPNAW::Mix(Sample *buffer, int32_t nsamples)
{
	(void)buffer; (void)nsamples;
}

void OPNAW::ClearBuffer(void)
{
}

}	// namespace FM
