//=============================================================================
//	opna_hw.h - Hardware OPNA backend for the pmdmini (PMDWin) sequencer
//
//	This replaces the ymfm-based FM::OPNA / FM::OPNAW classes when the build
//	defines PMDMINI_HW_BACKEND.  Register writes go straight to the real
//	PC-9801-86 (YM2608) through the tools/opna.c low-level driver; there is
//	no software synthesis and no mixing.  The sequencer (pmdwincore.cpp) is
//	unchanged and drives this class via the exact same opna->SetReg() calls
//	that the offline ymfm renderer uses.
//
//	Write-only registers (everything except the ADPCM data port 0x108 and
//	the status register) are shadowed in software because the real chip
//	cannot read them back, while PMD reads e.g. the SSG mixer (0x07) with a
//	read-modify-write.
//
//	SPDX-License-Identifier: GPL-2.0-or-later (matches pmdmini)
//=============================================================================

#ifndef PMDMINI_OPNA_HW_H
#define PMDMINI_OPNA_HW_H

#include "portability_fmpmd.h"		// TCHAR, uint{8..64}_t, WINAPI, _MAX_PATH
#include "ifileio.h"			// IFILEIO

#define FM_SAMPLETYPE	int32_t

namespace FM {

typedef int32_t Sample;

class OPNA
{
public:
	OPNA(IFILEIO *pfileio);
	virtual ~OPNA();

	void setfileio(IFILEIO *pfileio);

	bool Init(uint32_t c, uint32_t r, bool ip = false, const TCHAR *path = NULL);
	bool SetRate(uint32_t r);
	bool SetRate(uint32_t c, uint32_t r, bool = false);
	bool LoadRhythmSample(const TCHAR *);
	void Reset();

	void SetVolumeFM(int db);
	void SetVolumePSG(int db);
	void SetVolumeADPCM(int db);
	void SetVolumeRhythmTotal(int db);
	void SetVolumeRhythm(int index, int db);

	void SetReg(uint32_t addr, uint32_t data);
	uint32_t GetReg(uint32_t addr);

	uint32_t ReadStatus();
	uint32_t ReadStatusEx();

	bool Count(uint32_t us);
	uint32_t GetNextEvent();

	void Mix(Sample *buffer, int nsamples);

protected:
	IFILEIO *pfileio;
};

//	OPNAW is the "OPNA with wait" wrapper used by PMDWIN.  In hardware mode
//	the wait machinery is meaningless (the chip runs in real time), so the
//	methods degenerate to passthroughs / no-ops that keep the sequencer
//	interface intact.
class OPNAW : public OPNA
{
public:
	OPNAW(IFILEIO *pfileio);
	virtual ~OPNAW();
	void setfileio(IFILEIO *pfileio);

	bool Init(uint32_t c, uint32_t r, bool ipflag, const TCHAR *path);
	bool SetRate(uint32_t c, uint32_t r, bool ipflag = false);

	void SetFMWait(int32_t nsec);
	void SetSSGWait(int32_t nsec);
	void SetRhythmWait(int32_t nsec);
	void SetADPCMWait(int32_t nsec);

	int32_t GetFMWait(void);
	int32_t GetSSGWait(void);
	int32_t GetRhythmWait(void);
	int32_t GetADPCMWait(void);

	void SetReg(uint32_t addr, uint32_t data);
	void Mix(Sample *buffer, int32_t nsamples);
	void ClearBuffer(void);

private:
	int32_t fmwait;
	int32_t ssgwait;
	int32_t rhythmwait;
	int32_t adpcmwait;
};

}	// namespace FM

#endif	// PMDMINI_OPNA_HW_H
