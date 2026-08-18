# PMD98 .M decoder — full code-path report + cross-reference with pc98snd

Compiled 2026-08 from PMD48s (`/tmp/pmd/source/pmd48s/PMD.ASM`, ~10800 lines)
plus two independent subsystem analyses (PSG/rhythm, timing/LFO). Line numbers
are from the decoded PMD.ASM. Cross-referenced against `tools/pmd.c`,
`tools/opna.c` of this repo.

---

## 1. File structure and part model

- Byte 0: x68 flag. Bytes 1..22: 11 little-endian offsets (parts 1-11:
  FM1-6, SSG1-3, rhythm, ADPCM/effect). Byte 23: `2*(max_part2+1)` probe;
  bytes 24-26 belong to the "program" extension (used only when byte 23
  equals that constant). Part data at `file + 1 + offset`; a part whose
  first byte is 0x80 is never played (`play_init` @ 505: `address[di]=0`).
- Part work area defaults (`play_init`):
  - FM: `volume=108`, `fmpan=0xC0`, `slotmask=0xF0`, `neiromask=0xFF`.
  - SSG: `volume=8`, `psgpat=7` (tone only), `envf=3` (no envelope until a
    0xF0/0xCD command).
  - `leng=1` initially (first event after one tick), `onkai=-1` (rest),
    `mdc=-1` (depth counter infinite).
- Per-tick part order (`mmain` @ 900): SSG1, SSG2, SSG3, FM4/5/6 (bank 2),
  FM1, FM2, FM3, FM3-ext1/2/3, rhythm.

## 2. Command dispatch and the 0xB1 gate

Dispatch (`command00` @ 1724): `cmp al, com_end` (`com_end equ 0b1h`);
**any opcode < 0xB1 ends the part** — the byte is overwritten with 0x80 in
the stream and the part stops (FM/PSG/rhythm tables all share this gate).
Index = `(0xFF - opcode) * 2` into `cmdtbl` (FM, @ 1755), `cmdtblp` (PSG,
@ 1843), `cmdtblr` (rhythm, @ 1894). Unused slots are `jumpN` stubs that
still consume N bytes, so the parser never desyncs on them.

### 2.1 FM parameter counts (verified from handler bodies)

| op | handler | params | op | handler | params |
|----|---------|--------|----|---------|--------|
| FF | com@ (voice) | 1 | C0 | ssg_mml_part_mask | 1 |
| FE | comq | 1 | BF | _lfoset | 4 |
| FD | comv | 1 | BE | _lfoswitch | 1 |
| FC | comt | 1 or 3 | BD | _mdepth_set | 2 |
| FB | comtie | 0 | BC | _lfowave_set | 1 |
| FA | comd | 2 | BB | _lfo_extend | 1 |
| F9 | comstloop | 2 | BA | jump1 | 1 |
| F8 | comedloop | 4 | B9 | _lfoset_delay | 1 |
| F7 | comexloop | 2 | B8 | tl_set | 1 |
| F6 | comlopset | 0 | B7 | mdepth_count | 1 |
| F5 | comshift | 1 | B6 | fb_set | 1 |
| F4 | comvolup | 0 | B5 | slot_delay | 1 |
| F3 | comvoldown | 0 | B4 | jump16 | 16 |
| F2 | lfoset | 4 | B3 | comq3 | 1 |
| F1 | lfoswitch_f | 1 | B2 | comshift_master | 1 |
| F0 | jump4 | 4 | B1 | comq4 | 1 |
| EF | comy (raw reg) | 2 | CE | jump6 | 6 |
| ED,EE | jump1 | 1 | CD | extend_psgenvset | 5 |
| EC | panset | 1 | CC | detune_extend | 2 |
| EB | rhykey | 1 | CB | lfowave_set | 1 |
| EA | rhyvs | 1 | CA | lfo_extend | 1 |
| E9 | rpnset | 1 | C9 | envelope_extend | 4 |
| E8 | rmsvs | 1 | C8,C7 | jump3 | 3 |
| E7 | comshift2 | 1 | C6 | jump6 | 6 |
| E6 | rmsvs_sft | 1 | C5 | jump1 | 1 |
| E5 | rhyvs_sft | 2 | C4 | comq2 | 2 |
| E4 | hlfo_delay | 1 | C3 | jump2 | 2 |
| E3 | comvolup2 | 1 | C2 | lfoset_delay | 1 |
| E2 | comvoldown2 | 1 | C1 | jump0 | 0 |
| E1 | hlfo_set | 1 | D6 | mdepth_set | 2 |
| E0 | hlfo_onoff | 1 | D5 | comdd | 2 |
| DF | syousetu_lng_set | 1 | D4 | ssg_efct_set | 1 |
| DE | vol_one_up | 0 | D3 | fm_efct_set | 1 |
| DD | vol_one_down | 1 | D2 | fade_set | 1 |
| DC | status_write | 1 | D0 | psgnoise_move | 1 |
| DB | status_add | 1 | D9-D7,D1,CF | jump1 | 1 |
| DA | porta | 3 | CC | detune_extend | 2 |

### 2.2 PSG table differences (cmdtblp)

F0 = `psgenvset` (4), F4 = `comvolupp` (0, vol+1 clamp 15), F3 =
`comvoldownp` (0, vol-1), F1 = `lfoswitch` (1: any value >7 → 1, bits 0-2),
EE = `psgnoise` (1), ED = `psgsel` (1), E3 = `comvolupp2` (1, +n clamp 15),
E2 = `comvoldownp2` (1, -n), DE = `vol_one_up_psg` (1, volpush), DD =
`vol_one_down` (1, volpush), DA = `portap` (3), D0 = `psgnoise_move` (1),
CD = `extend_psgenvset` (5), CC = `detune_extend` (1), CA = `lfo_extend` (1,
extendmode bit1 = Timer-A sync), C9 = `envelope_extend` (1, extendmode bit2),
C0 = `ssg_mml_part_mask` (1: 0 = unmask, 1 = mask, ≥2 = special vd_* table).

The `_lfo*` opcodes (B9-BF) route through `_lfo_main` @ 3722
(`call lfo_change; call ax; call lfo_change`) — they call the base handler,
so **they consume the base handler's parameter bytes** (4/1/2/1/1/1 for
BF/BE/BD/BC/BB/B9).

## 3. The note path (FM)

`fmmain` @ 1053 per tick: `leng--`; if remaining ≤ `qdat` and not yet keyed
off → `keyoff`; if `leng != 0` done. Else fetch bytes: `< 0x80` note,
`== 0x80` end (honours `partloop` "L"), `> 0x80` command.

Note byte: bits 6-4 = block, bits 3-0 = scale index (0x0F = rest). On a note
(`mp2` @ 1089): `lfoinit` → `oshift` (shift + shift_def, octave carry) →
`fnumset` → length byte → `calc_q` (gate) → tie check (0xFB follows) →
`volset` (carrier TLs) → `otodasi` (fnum write) → `keyon` (0x28).

- `fnum_data` @ 7925 (block 0): C 0x26A, C# 0x28F, D 0x2B6, D# 0x2DF, E
  0x30B, F 0x339, F# 0x36A, G 0x39E, G# 0x3D5, A 0x410, A# 0x44E, B 0x48F.
- `fnum = fnum_data[onkai] | (block << 10)`.
- `otodasi` @ 4335: `fnum + porta_num + detune + lfodat(bit0) + _lfodat(bit4)`,
  then `fm_block_calc` @ 4549: **octave renormalisation** — while
  `fnum >= 0x4D2`: block++, fnum -= 0x26A; while `fnum < 0x26A` and block>0:
  block--, fnum += 0x26A; clamps at block 7 (fnum ≤ 0x7FF) / block 0.
  Write reg `A4+part` = block|fnum-hi, `A0+part` = fnum-lo.
- Key-on/off: reg 0x28 with the channel's accumulated slot-mask
  (`omote_key1..3` per channel; keyoff ANDs the complement). Rest: no write.

## 4. The voice (tone data) path — neiroset + fmvs

`toneadr_calc` @ 5301: address = `tondat + voice_number × 32`. The 26-byte
entry: bytes 0-23 = six 4-byte groups (DT/MUL, TL, KS/AR, AM/DR, SR,
SL/RR), byte 24 = ALG/FB, byte 25 = id.

`neiroset_main` @ 5159 writes the voice to the chip:
- ALG/FB byte 24 → reg B0+part (`alg_fb`). Carrier bits for the volume from
  `carrier_table` @ 7958:
  `{0x80,0x80,0x80,0x80, 0xA0,0xE0,0xE0,0xF0}` (algs 0-7) — **the bits are
  in the order [slot4, slot2, slot3, slot1]: bit7=slot4, bit6=slot2,
  bit5=slot3, bit4=slot1** (i.e. bit7..bit4 = file op4, op3, op2, op1 in
  *hardware-slot* positions 4,2,3,1). The +8 table (TL mask, "slot2/3
  逆転"): `{0xEE×4, 0xCC, 0x88, 0x88, 0x00}` — blocks the neiroset TL writes
  on the carrier slots (bits in the *normal* order for the neiroset loop).
- DT/MUL (bytes 0-3) → regs 30,34,38,3C; TL (bytes 4-7) → 40,44,48,4C;
  KS/AR (8-11) → 50..; AM/DR (12-15) → 60..; SR (16-19) → 70..; SL/RR
  (20-23) → 80..; **the plain file order: file byte i → hardware slot i
  (so MML op3 lands in hw slot 2 and MML op2 in hw slot 3)**. Masked by
  `neiromask` (the "other" groups) and the TL mask (TL group).
- The TL group (bytes 4-7) is also copied to the per-part work vars
  `slot1..slot4[di]` in file order (movsw ×2).

`volset` @ 4726 → `fmvs` @ 4770 (runs at every key-on and every tick the
volume LFO/fadeout is active): volume `cl = 255 - volume` (after voldown /
fadeout scaling), builds `vol_tbl = {0x80,0x80,0x80,0x80}`, sets
`vol_tbl[j] = cl` for each carrier bit (bit7→vol_tbl[0] … bit4→vol_tbl[3]),
then **rewrites only the carrier slots' TLs** with the pairing:

```
reg 0x4C (slot4) = vol_tbl[0] + slot4[di] (file byte 7 = MML op4 TL)
reg 0x44 (slot2) = vol_tbl[1] + slot3[di] (file byte 6 = MML op2 TL)   [reversed]
reg 0x48 (slot3) = vol_tbl[2] + slot2[di] (file byte 5 = MML op3 TL)   [reversed]
reg 0x40 (slot1) = vol_tbl[3] + slot1[di] (file byte 4 = MML op1 TL)
```

with `TL = clamp(clamp(vol_tbl[j] + TL) - 0x80)`. Non-carrier slots keep
the neiroset TLs (file order). Net effect: for algs 0-3 (carrier = slot 4
only) the voice is **pure file order**; for multi-carrier algs (4-7) the
carrier slots' TLs use the swapped (MML-order) bytes. The volume offset =
`127 - volume` on the carrier slots.

The tone editor (ymtone) writes the same file-order mapping (op[i] → slots
{1,3,2,4} via OP_OFFS {0,2,1,3} + write offsets {0,8,4,12}) with consistent
per-slot parameters — matching the neiroset's identity.

## 5. Timing

- One OPNA Timer-B interrupt = one tick; `settempo_b` writes reg 0x26.
  Timer-B period = `(256 - tempo_d) × 2304 / 7.9872 MHz` (≈ 0.288 ms ×
  (256 - tempo_d)). Default `tempo_d = 200` @ 6011.
- `t` (FC FF nn): `tempo_d = 256 - round(4396 / nn)` (`calc_tempo_tb`
  @ 3502, guard nn ≥ 18). `T` (FC nn < 0xFB): `tempo_d = nn` directly.
- Timer A (9216 µs fixed) drives fadeout (every 8th A-int), effects, keys.

## 6. LFO

- `lfoswi` bits: 0 pitch LFO1, 1 volume LFO1, 2 async, 3 porta, 4 pitch
  LFO2, 5 volume LFO2.
- `lfoset` 0xF2 (4): delay, speed, step, time — sets LFO1 and LFO2 alike.
  `lfoset_delay` 0xC2 (1): delay only. `lfowave_set` 0xCB (1): wave 0/4/5
  triangle, 1 saw, 2 square, 3 random, 6 one-shot. `mdepth_set` 0xD6 (2):
  mdspd, mdepth. `mdepth_count` 0xB7 (1): mdc. `lfo_extend` 0xCA (1):
  extendmode bit1 (Timer-A sync).
- `lfo` @ 5341: count down delay; then count down speed (speed==1 → step,
  speed==-1 → frozen, else --speed); on a step, per-wave update of `lfodat`
  with time-based reversal; `md_inc` deepens `|step|` by `mdepth` at
  `mdspd` intervals while `mdc` counts. `lfo_change` @ 3769 swaps LFO1↔LFO2
  params (used when both LFOs active).
- Pitch: `otodasi` adds `lfodat` (+ `_lfodat`) to the fnum. Volume: `fmvs`
  calls `fmlfo_sub` (subtract |lfodat| from the carrier vol_tbl entries,
  clamped) — **the volume LFO (tremolo)**.
- `lfoinit_main` @ 5737 at each note: `lfodat=0`, delay/step reloaded from
  the shadow, speed set to 1 (waves 2/3) or speed+1 (others) so the LFO
  starts right after the delay.

## 7. Volume / envelope / SSG

- FM volume: `volset` (see §4); global `fm_voldown` (0xE3/0xE2 relative
  vol) and `fadeout_volume` (0xD2, Timer-A driven, ≤ 50% cut) scale `cl`.
- `volpush` (0xDE/0xDD, 1-note volume push): consumed by the next note,
  then cleared.
- SSG: `psgenvset` 0xF0 (4): pat/patb, pv2, pr1/pr1b, pr2/pr2b. `envf`:
  0 attack (count pat, then jump to pv2), 1 decay (pr1 ticks per step),
  2 release (pr2 ticks per step, 0 = silent), 3 none. `penv` -15..15.
  Per-tick `volsetp`: reg 0x08-0x0A = clamp(volume + penv, 0, 15) (+volume
  LFO). Extended env (0xCD, 5 params, envf=-1) with eenv phases.
- `keyonp`: mixer 0x07 read-modify-write (channel mask 0x09/0x12/0x24 ANDed
  with `psgpat`); noise period 0x06 written only on change. **Keyoff never
  touches the mixer** — the envelope releases to silence.
- Rhythm: regs 0x10 (keyon), 0x11 (total level), 0x18-0x1D (per-drum
  control); pattern stream = 2-byte drum mask + 1 length byte; 11 drums
  (rhydat @ 8014); two-phase C-HH write (0x84 then 0x08).

## 8. Loops and control flow

- `[` = F9 (2): zero the counter byte at `mmlbuf + offset + 1`.
- `]` = F8 (4): count, current, offset — self-modifying counter in the
  stream; loop back to `mmlbuf + offset + 2` while current != count.
- F7 comexloop (2): exit-loop check (dec counter, compare).
- F6 comlopset (0 params!): `partloop = si` — the "L" jump target used at
  the 0x80 end-of-part.
- 0x80 end-of-part: `si--`, write 0x80 back; if partloop set, jump there.
- Tie: 0xFB directly after a note's length → skip auto-keyoff
  (`keyoff_flag = 2`).

---

## 9. Cross-reference with pc98snd — what we do and where we differ

### 9.1 Verified identical

- Header/offset parsing, 0x80-not-played, 11 parts, default volumes
  (FM 108 / SSG 8), pan 0xC0, slotmask 0xF0.
- Command dispatch gate (`< 0xB1` ends the part) and both param tables
  (re-derived from the handlers; the `_lfo*` aliases now consume the base
  counts).
- fnum table byte-exact; note → fnum/block; the 0xFB tie; Q-gate keyoff.
- neiroset voice programming: file byte i → hw slot i, ALG/FB at byte 24,
  carrier_table bit order [slot4, slot2, slot3, slot1].
- fmvs: carrier-only TL rewrite with the slot-2/3 reversal pairing;
  non-carrier slots keep the file order; offset = 127 - volume.
- Tempo formulas, timer-B tick, `t`/`T` special cases, the 4396 constant.
- LFO: 4-param lfoset, waves 0/1/2/3/4/5/6, time reversal, mdepth/mdc,
  per-tick pitch rewrite via `fm_block_calc` octave carry.
- Portamento (3-byte, per-tick step + remainder), loops (F8/F9/F7), the
  "L" part-loop, SSG software envelope (pat/pv2/pr1/pr2), SSG noise via
  psgsel/psgnoise, SSG mixer read-modify-write (software shadow).
- Rhythm part parsed; SSG drums; reset hygiene; busy-flag polling.

### 9.2 Fixed this round (with the user's "instruments off" complaint)

1. **`_lfo*` alias parameter counts** (B9/BB/BC/BD/BE/BF): were 0, now
   1/1/1/2/1/4 — a parser desync that turned `bb 01` into a C#0 drone in
   th04-13 (4 s of 34.7 Hz across all six FM channels).
2. **Master transpose 0xB2** (shift_def) — was parsed but never applied;
   now added to the per-part shift at note-on.
3. **fmvs TL model**: the previous code swapped slots 2/3 for *all* slots.
   PMD only rewrites the **carrier** slots (bit7=hw4, bit6=hw2, bit5=hw3,
   bit4=hw1) with the reversed byte pairing; non-carrier slots keep the
   neiroset (file-order) TLs. Algs 0-3 are therefore pure file order.
4. **fm_block_calc octave carry**: the fnum + porta + detune + lfodat sum
   was clamped to [0, 0x3FF]; PMD carries the octave at the 0x26A/0x4D2
   boundaries (block±1, fnum±0x26A) with block 0/7 caps. The clamp bent
   pitches flat at vibrato/glide extremes — a real "detuned" source.
5. **SSG mixer shadow**: `opna_ssg_note` wrote `0x3F & ~bit` — muting the
   other SSG channels on every key-on; now a software read-modify-write
   keeps the other channels' mixer bits, like `keyonp`.
6. **0xBD (_mdepth_set)** consumed 0 of its 2 params — desync; now 2.

### 9.3 Remaining differences (accepted or low-impact so far)

| Area | PMD | pc98snd | Impact |
|------|-----|---------|--------|
| Volume LFO (tremolo) | `fmvs` + `fmlfo_sub` subtract \|lfodat\| from carrier TLs every tick when lfoswi bit1/5 | not implemented (bit1 parsed, ignored) | audible on songs using the MML volume LFO |
| Dual LFO (LFO2) | `lfo_change` swaps LFO1↔LFO2; `_lfodat` added to pitch (bit4) | single LFO; `_`-commands consume but don't set shadow | second-layer LFO absent |
| lfoinit at note-on | reloads delay/step from shadow, resets speed | resets lfodat only | LFO phase restarts differ |
| volpush 0xDE/0xDD | 1-note volume push | treated as permanent volume change | one-note accents wrong |
| Fadeout 0xD2 | Timer-A driven global fade (≤50%) | not implemented | end-of-song fades |
| Hardware LFO 0xE0/0xE1 | 0x22/0xB4 AMS/PMS writes | not implemented | songs using the hw LFO |
| comy 0xEF bank | writes the *part's* bank (bank2 for FM4-6) | always bank 2 for FM parts | raw-register songs on FM4-6 |
| qdat2/qdat3 (0xC4/B3/B1) | gate guarantee / random Q | params consumed, ignored | gate length edge cases |
| extendmode (Timer-A sync, 0xCA bit1) | LFO/soft-env synced to Timer A | ignored | LFO rate on `ca 1` songs |
| PSG tone+noise mix (psgpat) | both bits can be on | noise bit wins | mix-mode voices |
| SSG detune (SSG `fa`) | subtracts detune from period | not applied | SSG detune voices |
| Rhythm pattern mode | full rhythm driver (0x10/0x11/0x18-1D) | SSG drums only | OPNA rhythm songs |

### 9.4 Verified on real hardware this round

- e008 voice id=31: `b0=3d` (alg 5 fb 7), DT/MUL `02 02 04 02`, TLs
  `19 06 06 05`, KS/AR `15×4`, SL/RR `00 09 09 09` — byte-exact vs the file.
- First keyon: fnum 0x28F block 3 (C#4 = 277 Hz), carrier TL 13, keyon
  `28=f2` — the frequencies in the log map to the correct notes (C#4, B4,
  G#5, F#5, A5…), no tuning error.
- Pan registers 0xB4-0xB6 = 0xC0 (centre).
