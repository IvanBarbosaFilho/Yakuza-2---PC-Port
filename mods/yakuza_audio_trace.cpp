// Yakuza (SLUS_213.48): CRI ADX stereo fix, plus a diagnostic trace.
//
// The fix (on unless PS2X_ADX_STEREO_FIX=0). The EE ADX decoder's start (FUN_ovl_main__00202338) decides whether a
// block fits by asking only the FIRST output channel's ring for its free space (0x2025d0: sj->vtbl+0x24(sj, 0)), then
// decodes every channel; at the end (FUN_ovl_main__002026a0) each channel's output chunk is cut to the decoded size,
// so when the R ring had less free space than L the rest of the R samples was dropped. On the PS2 both rings drain at
// the same pace; in the port L and R drift apart by a few ms at times (per-channel EE -> IOP sends), R lost audio for
// good: an R channel short by ~0xb40 bytes in the IOP for the rest of the stream (Ivan's cutscene, port dumps vs a
// PCSX2 savestate, HANDOFF 2026-11-16), heard as cuts and a late right channel. Here that one call answers the
// smallest free space of all the decoder's output channels, so a block is decoded only when it fits in every one.
//
// The trace (PS2X_TRACE_SJPUT=1): every PutChunk on CRI's EE ring buffers (FUN_ovl_main__0021f1f8(sj, line, chunk*))
// to sjput.txt with the vsync, ring, line, chunk address and size, and the caller.
#include "game_overrides.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
    constexpr uint32_t kPutChunkFn = 0x0021f1f8u;   // SJRBF vtbl+0x20
    constexpr uint32_t kNumDataFn = 0x0021efa8u;    // SJRBF vtbl+0x24: line 1 -> +0xc, line 0 -> +0x10
    constexpr uint32_t kFreeCheckReturn = 0x002025d8u; // after the free-space call in FUN_ovl_main__00202338
    constexpr uint32_t kOutputChunksFn = 0x00202218u;  // gets each channel's output chunk, sizes the decode by ch 0's
    std::FILE *g_out = nullptr;
    uint64_t g_adjusted = 0u;

    uint32_t readWord(const uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0u;
        std::memcpy(&value, rdram + (address & 0x1FFFFFCu), sizeof(value));
        return value;
    }

    bool inRam(uint32_t address) { return (address & 0x1FFFFFFu) >= 0x100000u && (address & 0x1FFFFFFu) < 0x1FF0000u; }

    void callOriginal(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (PS2Runtime::RecompiledFunction original = runtime->overlayOriginal(address))
            original(rdram, ctx, runtime);
    }

    void putChunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t sj = GPR_U32(ctx, 4), line = GPR_U32(ctx, 5), chunk = GPR_U32(ctx, 6), ra = GPR_U32(ctx, 31);
        if (g_out && chunk)
            std::fprintf(g_out, "v=%llu sj=%x line=%u ptr=%x size=%x ra=%x\n",
                         static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load()), sj, line,
                         readWord(rdram, chunk), readWord(rdram, chunk + 4u), ra);
        callOriginal(kPutChunkFn, rdram, ctx, runtime);
    }

    void numData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31), line = GPR_U32(ctx, 5), first = GPR_U32(ctx, 4);
        callOriginal(kNumDataFn, rdram, ctx, runtime);
        if (ra != kFreeCheckReturn || line != 0u)
            return;
        // $s2 = the decoder (FUN_ovl_main__00202338's iVar10): output rings at +0xc + 4 * channel, the ADXB at +4.
        const uint32_t decoder = GPR_U32(ctx, 18);
        const uint32_t adxb = readWord(rdram, decoder + 4u);
        if (!inRam(decoder) || !inRam(adxb) || readWord(rdram, decoder + 0xcu) != first)
            return;
        // FUN_ovl_main__001f7d20: the channel count.
        const uint8_t declared = rdram[(adxb + 0xeu) & 0x1FFFFFFu];
        const uint32_t channels = (declared == 1u && readWord(rdram, adxb + 0xe8u) != 0u) ? 2u : declared;
        const uint32_t vtable = readWord(rdram, first);
        int32_t smallest = static_cast<int32_t>(GPR_U32(ctx, 2));
        for (uint32_t channel = 1u; channel < std::min(channels, 8u); ++channel)
        {
            const uint32_t sj = readWord(rdram, decoder + 0xcu + 4u * channel);
            if (!inRam(sj) || readWord(rdram, sj) != vtable)
                return; // another kind of ring: leave the game's answer
            smallest = std::min(smallest, static_cast<int32_t>(readWord(rdram, sj + 0x10u)));
        }
        if (smallest != static_cast<int32_t>(GPR_U32(ctx, 2)))
        {
            if (g_adjusted++ < 20u)
                std::cerr << "[adx] stereo decode held back: ch0 free 0x" << std::hex << GPR_U32(ctx, 2)
                          << ", smallest 0x" << smallest << std::dec << " (" << channels << " channels)" << std::endl;
            SET_GPR_S32(ctx, 2, smallest);
        }
    }

    // FUN_ovl_main__00202218(decoder, &a, &samples, &c): GetChunk(line 0, 0x4000) on every output ring (chunks at
    // decoder + 0x1c + 8 * channel: address, size), then samples = min(channel 0's chunk / 2, decoder + 0x38). A
    // shorter chunk on another channel (its ring wraps sooner once L and R drifted) lost the rest of that channel's
    // samples at the end of the decode. The decode is cut to the shortest chunk.
    void outputChunks(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t decoder = GPR_U32(ctx, 4), samplesAt = GPR_U32(ctx, 6);
        callOriginal(kOutputChunksFn, rdram, ctx, runtime);
        const uint32_t adxb = readWord(rdram, decoder + 4u);
        if (!inRam(decoder) || !inRam(adxb) || !inRam(samplesAt))
            return;
        const uint8_t declared = rdram[(adxb + 0xeu) & 0x1FFFFFFu];
        const uint32_t channels = (declared == 1u && readWord(rdram, adxb + 0xe8u) != 0u) ? 2u : declared;
        const int32_t planned = static_cast<int32_t>(readWord(rdram, samplesAt));
        int32_t samples = planned;
        for (uint32_t channel = 1u; channel < std::min(channels, 8u); ++channel)
            samples = std::min(samples, static_cast<int32_t>(readWord(rdram, decoder + 0x20u + 8u * channel)) / 2);
        if (samples < planned)
        {
            if (g_adjusted++ < 40u)
                std::cerr << "[adx] stereo decode cut to the shortest output chunk: " << planned << " -> " << samples
                          << " samples" << std::endl;
            const int32_t value = std::max(samples, 0);
            std::memcpy(rdram + (samplesAt & 0x1FFFFFFu), &value, sizeof(value));
        }
    }

    void apply(PS2Runtime &runtime)
    {
        const char *fix = std::getenv("PS2X_ADX_STEREO_FIX");
        if (!fix || fix[0] != '0')
        {
            runtime.replaceFunction(kNumDataFn, &numData);
            runtime.replaceFunction(kOutputChunksFn, &outputChunks);
            std::cerr << "[adx] stereo free-space fix on (PS2X_ADX_STEREO_FIX=0 turns it off)" << std::endl;
        }
        const char *trace = std::getenv("PS2X_TRACE_SJPUT");
        if (trace && trace[0] == '1')
        {
            g_out = std::fopen("sjput.txt", "w");
            runtime.replaceFunction(kPutChunkFn, &putChunk);
        }
    }
}

PS2_REGISTER_GAME_OVERRIDE("Yakuza CRI ADX stereo fix", "SLUS_213.48", 0u, 0u, apply)
