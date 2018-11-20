/**
 * OpenAL cross platform audio library
 * Copyright (C) 1999-2007 by authors.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "AL/al.h"
#include "AL/alc.h"

#include "alMain.h"
#include "alcontext.h"
#include "alSource.h"
#include "alBuffer.h"
#include "alListener.h"
#include "alAuxEffectSlot.h"
#include "sample_cvt.h"
#include "alu.h"
#include "alconfig.h"
#include "ringbuffer.h"

#include "cpu_caps.h"
#include "mixer/defs.h"


static_assert((INT_MAX>>FRACTIONBITS)/MAX_PITCH > BUFFERSIZE,
              "MAX_PITCH and/or BUFFERSIZE are too large for FRACTIONBITS!");

/* BSinc24 requires up to 23 extra samples before the current position, and 24 after. */
static_assert(MAX_RESAMPLE_PADDING >= 24, "MAX_RESAMPLE_PADDING must be at least 24!");


enum Resampler ResamplerDefault = LinearResampler;

MixerFunc MixSamples = Mix_C;
RowMixerFunc MixRowSamples = MixRow_C;
static HrtfMixerFunc MixHrtfSamples = MixHrtf_C;
static HrtfMixerBlendFunc MixHrtfBlendSamples = MixHrtfBlend_C;

static MixerFunc SelectMixer(void)
{
#ifdef HAVE_NEON
    if((CPUCapFlags&CPU_CAP_NEON))
        return Mix_Neon;
#endif
#ifdef HAVE_SSE
    if((CPUCapFlags&CPU_CAP_SSE))
        return Mix_SSE;
#endif
    return Mix_C;
}

static RowMixerFunc SelectRowMixer(void)
{
#ifdef HAVE_NEON
    if((CPUCapFlags&CPU_CAP_NEON))
        return MixRow_Neon;
#endif
#ifdef HAVE_SSE
    if((CPUCapFlags&CPU_CAP_SSE))
        return MixRow_SSE;
#endif
    return MixRow_C;
}

static inline HrtfMixerFunc SelectHrtfMixer(void)
{
#ifdef HAVE_NEON
    if((CPUCapFlags&CPU_CAP_NEON))
        return MixHrtf_Neon;
#endif
#ifdef HAVE_SSE
    if((CPUCapFlags&CPU_CAP_SSE))
        return MixHrtf_SSE;
#endif
    return MixHrtf_C;
}

static inline HrtfMixerBlendFunc SelectHrtfBlendMixer(void)
{
#ifdef HAVE_NEON
    if((CPUCapFlags&CPU_CAP_NEON))
        return MixHrtfBlend_Neon;
#endif
#ifdef HAVE_SSE
    if((CPUCapFlags&CPU_CAP_SSE))
        return MixHrtfBlend_SSE;
#endif
    return MixHrtfBlend_C;
}

ResamplerFunc SelectResampler(enum Resampler resampler)
{
    switch(resampler)
    {
        case PointResampler:
            return Resample_point_C;
        case LinearResampler:
#ifdef HAVE_NEON
            if((CPUCapFlags&CPU_CAP_NEON))
                return Resample_lerp_Neon;
#endif
#ifdef HAVE_SSE4_1
            if((CPUCapFlags&CPU_CAP_SSE4_1))
                return Resample_lerp_SSE41;
#endif
#ifdef HAVE_SSE2
            if((CPUCapFlags&CPU_CAP_SSE2))
                return Resample_lerp_SSE2;
#endif
            return Resample_lerp_C;
        case FIR4Resampler:
            return Resample_cubic_C;
        case BSinc12Resampler:
        case BSinc24Resampler:
#ifdef HAVE_NEON
            if((CPUCapFlags&CPU_CAP_NEON))
                return Resample_bsinc_Neon;
#endif
#ifdef HAVE_SSE
            if((CPUCapFlags&CPU_CAP_SSE))
                return Resample_bsinc_SSE;
#endif
            return Resample_bsinc_C;
    }

    return Resample_point_C;
}


void aluInitMixer(void)
{
    const char *str;

    if(ConfigValueStr(NULL, NULL, "resampler", &str))
    {
        if(strcasecmp(str, "point") == 0 || strcasecmp(str, "none") == 0)
            ResamplerDefault = PointResampler;
        else if(strcasecmp(str, "linear") == 0)
            ResamplerDefault = LinearResampler;
        else if(strcasecmp(str, "cubic") == 0)
            ResamplerDefault = FIR4Resampler;
        else if(strcasecmp(str, "bsinc12") == 0)
            ResamplerDefault = BSinc12Resampler;
        else if(strcasecmp(str, "bsinc24") == 0)
            ResamplerDefault = BSinc24Resampler;
        else if(strcasecmp(str, "bsinc") == 0)
        {
            WARN("Resampler option \"%s\" is deprecated, using bsinc12\n", str);
            ResamplerDefault = BSinc12Resampler;
        }
        else if(strcasecmp(str, "sinc4") == 0 || strcasecmp(str, "sinc8") == 0)
        {
            WARN("Resampler option \"%s\" is deprecated, using cubic\n", str);
            ResamplerDefault = FIR4Resampler;
        }
        else
        {
            char *end;
            long n = strtol(str, &end, 0);
            if(*end == '\0' && (n == PointResampler || n == LinearResampler || n == FIR4Resampler))
                ResamplerDefault = static_cast<enum Resampler>(n);
            else
                WARN("Invalid resampler: %s\n", str);
        }
    }

    MixHrtfBlendSamples = SelectHrtfBlendMixer();
    MixHrtfSamples = SelectHrtfMixer();
    MixSamples = SelectMixer();
    MixRowSamples = SelectRowMixer();
}


namespace {

static void SendAsyncEvent(ALCcontext *context, ALuint enumtype, ALenum type,
                           ALuint objid, ALuint param, const char *msg)
{
    AsyncEvent evt = ASYNC_EVENT(enumtype);
    evt.u.user.type = type;
    evt.u.user.id = objid;
    evt.u.user.param = param;
    strcpy(evt.u.user.msg, msg);
    if(ll_ringbuffer_write(context->AsyncEvents, &evt, 1) == 1)
        alsem_post(&context->EventSem);
}


/* Base template left undefined. Should be marked =delete, but Clang 3.8.1
 * chokes on that given the inline specializations.
 */
template<FmtType T>
inline ALfloat LoadSample(typename FmtTypeTraits<T>::Type val);

template<> inline ALfloat LoadSample<FmtUByte>(FmtTypeTraits<FmtUByte>::Type val)
{ return (val-128) * (1.0f/128.0f); }
template<> inline ALfloat LoadSample<FmtShort>(FmtTypeTraits<FmtShort>::Type val)
{ return val * (1.0f/32768.0f); }
template<> inline ALfloat LoadSample<FmtFloat>(FmtTypeTraits<FmtFloat>::Type val)
{ return val; }
template<> inline ALfloat LoadSample<FmtDouble>(FmtTypeTraits<FmtDouble>::Type val)
{ return (ALfloat)val; }
template<> inline ALfloat LoadSample<FmtMulaw>(FmtTypeTraits<FmtMulaw>::Type val)
{ return muLawDecompressionTable[val] * (1.0f/32768.0f); }
template<> inline ALfloat LoadSample<FmtAlaw>(FmtTypeTraits<FmtAlaw>::Type val)
{ return aLawDecompressionTable[val] * (1.0f/32768.0f); }

template<FmtType T>
inline void LoadSampleArray(ALfloat *RESTRICT dst, const void *src, ALint srcstep, ALsizei samples)
{
    using SampleType = typename FmtTypeTraits<T>::Type;

    const SampleType *ssrc = static_cast<const SampleType*>(src);
    for(ALsizei i{0};i < samples;i++)
        dst[i] += LoadSample<T>(ssrc[i*srcstep]);
}

static void LoadSamples(ALfloat *RESTRICT dst, const ALvoid *RESTRICT src, ALint srcstep,
                        enum FmtType srctype, ALsizei samples)
{
#define HANDLE_FMT(T)                                                         \
    case T: LoadSampleArray<T>(dst, src, srcstep, samples); break
    switch(srctype)
    {
        HANDLE_FMT(FmtUByte);
        HANDLE_FMT(FmtShort);
        HANDLE_FMT(FmtFloat);
        HANDLE_FMT(FmtDouble);
        HANDLE_FMT(FmtMulaw);
        HANDLE_FMT(FmtAlaw);
    }
#undef HANDLE_FMT
}


const ALfloat *DoFilters(BiquadFilter *lpfilter, BiquadFilter *hpfilter,
                         ALfloat *RESTRICT dst, const ALfloat *RESTRICT src,
                         ALsizei numsamples, int type)
{
    ALsizei i;
    switch(type)
    {
        case AF_None:
            BiquadFilter_passthru(lpfilter, numsamples);
            BiquadFilter_passthru(hpfilter, numsamples);
            break;

        case AF_LowPass:
            BiquadFilter_process(lpfilter, dst, src, numsamples);
            BiquadFilter_passthru(hpfilter, numsamples);
            return dst;
        case AF_HighPass:
            BiquadFilter_passthru(lpfilter, numsamples);
            BiquadFilter_process(hpfilter, dst, src, numsamples);
            return dst;

        case AF_BandPass:
            for(i = 0;i < numsamples;)
            {
                ALfloat temp[256];
                ALsizei todo = mini(256, numsamples-i);

                BiquadFilter_process(lpfilter, temp, src+i, todo);
                BiquadFilter_process(hpfilter, dst+i, temp, todo);
                i += todo;
            }
            return dst;
    }
    return src;
}

} // namespace

/* This function uses these device temp buffers. */
#define SOURCE_DATA_BUF 0
#define RESAMPLED_BUF 1
#define FILTERED_BUF 2
#define NFC_DATA_BUF 3
ALboolean MixSource(ALvoice *voice, ALuint SourceID, ALCcontext *Context, ALsizei SamplesToDo)
{
    ALCdevice *Device = Context->Device;
    ALbufferlistitem *BufferListItem;
    ALbufferlistitem *BufferLoopItem;
    ALsizei NumChannels, SampleSize;
    ALbitfieldSOFT enabledevt;
    ALsizei buffers_done = 0;
    ResamplerFunc Resample;
    ALsizei DataPosInt;
    ALsizei DataPosFrac;
    ALint64 DataSize64;
    ALint increment;
    ALsizei Counter;
    ALsizei OutPos;
    ALsizei IrSize;
    bool isplaying;
    bool firstpass;
    bool isstatic;
    ALsizei chan;
    ALsizei send;

    /* Get source info */
    isplaying      = true; /* Will only be called while playing. */
    isstatic       = !!(voice->Flags&VOICE_IS_STATIC);
    DataPosInt     = ATOMIC_LOAD(&voice->position, almemory_order_acquire);
    DataPosFrac    = ATOMIC_LOAD(&voice->position_fraction, almemory_order_relaxed);
    BufferListItem = ATOMIC_LOAD(&voice->current_buffer, almemory_order_relaxed);
    BufferLoopItem = ATOMIC_LOAD(&voice->loop_buffer, almemory_order_relaxed);
    NumChannels    = voice->NumChannels;
    SampleSize     = voice->SampleSize;
    increment      = voice->Step;

    IrSize = (Device->HrtfHandle ? Device->HrtfHandle->irSize : 0);

    Resample = ((increment == FRACTIONONE && DataPosFrac == 0) ?
                Resample_copy_C : voice->Resampler);

    Counter = (voice->Flags&VOICE_IS_FADING) ? SamplesToDo : 0;
    firstpass = true;
    OutPos = 0;

    do {
        ALsizei SrcBufferSize, DstBufferSize;

        /* Figure out how many buffer samples will be needed */
        DataSize64  = SamplesToDo-OutPos;
        DataSize64 *= increment;
        DataSize64 += DataPosFrac+FRACTIONMASK;
        DataSize64 >>= FRACTIONBITS;
        DataSize64 += MAX_RESAMPLE_PADDING*2;
        SrcBufferSize = (ALsizei)mini64(DataSize64, BUFFERSIZE);

        /* Figure out how many samples we can actually mix from this. */
        DataSize64  = SrcBufferSize;
        DataSize64 -= MAX_RESAMPLE_PADDING*2;
        DataSize64 <<= FRACTIONBITS;
        DataSize64 -= DataPosFrac;
        DstBufferSize = (ALsizei)mini64((DataSize64+(increment-1)) / increment,
                                        SamplesToDo - OutPos);

        /* Some mixers like having a multiple of 4, so try to give that unless
         * this is the last update. */
        if(DstBufferSize < SamplesToDo-OutPos)
            DstBufferSize &= ~3;

        /* It's impossible to have a buffer list item with no entries. */
        assert(BufferListItem->num_buffers > 0);

        for(chan = 0;chan < NumChannels;chan++)
        {
            const ALfloat *ResampledData;
            ALfloat *SrcData = Device->TempBuffer[SOURCE_DATA_BUF];
            ALsizei FilledAmt;

            /* Load the previous samples into the source data first, and clear the rest. */
            memcpy(SrcData, voice->PrevSamples[chan], MAX_RESAMPLE_PADDING*sizeof(ALfloat));
            memset(SrcData+MAX_RESAMPLE_PADDING, 0, (BUFFERSIZE-MAX_RESAMPLE_PADDING)*
                                                    sizeof(ALfloat));
            FilledAmt = MAX_RESAMPLE_PADDING;

            if(isstatic)
            {
                /* TODO: For static sources, loop points are taken from the
                 * first buffer (should be adjusted by any buffer offset, to
                 * possibly be added later).
                 */
                const ALbuffer *Buffer0 = BufferListItem->buffers[0];
                const ALsizei LoopStart = Buffer0->LoopStart;
                const ALsizei LoopEnd   = Buffer0->LoopEnd;
                const ALsizei LoopSize  = LoopEnd - LoopStart;

                /* If current pos is beyond the loop range, do not loop */
                if(!BufferLoopItem || DataPosInt >= LoopEnd)
                {
                    ALsizei SizeToDo = SrcBufferSize - FilledAmt;
                    ALsizei CompLen = 0;
                    ALsizei i;

                    BufferLoopItem = NULL;

                    for(i = 0;i < BufferListItem->num_buffers;i++)
                    {
                        const ALbuffer *buffer = BufferListItem->buffers[i];
                        const ALubyte *Data = static_cast<const ALubyte*>(buffer->data);
                        ALsizei DataSize;

                        if(DataPosInt >= buffer->SampleLen)
                            continue;

                        /* Load what's left to play from the buffer */
                        DataSize = mini(SizeToDo, buffer->SampleLen - DataPosInt);
                        CompLen = maxi(CompLen, DataSize);

                        LoadSamples(&SrcData[FilledAmt],
                            &Data[(DataPosInt*NumChannels + chan)*SampleSize],
                            NumChannels, buffer->FmtType, DataSize
                        );
                    }
                    FilledAmt += CompLen;
                }
                else
                {
                    ALsizei SizeToDo = mini(SrcBufferSize - FilledAmt, LoopEnd - DataPosInt);
                    ALsizei CompLen = 0;
                    ALsizei i;

                    for(i = 0;i < BufferListItem->num_buffers;i++)
                    {
                        const ALbuffer *buffer = BufferListItem->buffers[i];
                        const ALubyte *Data = static_cast<const ALubyte*>(buffer->data);
                        ALsizei DataSize;

                        if(DataPosInt >= buffer->SampleLen)
                            continue;

                        /* Load what's left of this loop iteration */
                        DataSize = mini(SizeToDo, buffer->SampleLen - DataPosInt);
                        CompLen = maxi(CompLen, DataSize);

                        LoadSamples(&SrcData[FilledAmt],
                            &Data[(DataPosInt*NumChannels + chan)*SampleSize],
                            NumChannels, buffer->FmtType, DataSize
                        );
                    }
                    FilledAmt += CompLen;

                    while(SrcBufferSize > FilledAmt)
                    {
                        const ALsizei SizeToDo = mini(SrcBufferSize - FilledAmt, LoopSize);

                        CompLen = 0;
                        for(i = 0;i < BufferListItem->num_buffers;i++)
                        {
                            const ALbuffer *buffer = BufferListItem->buffers[i];
                            const ALubyte *Data = static_cast<const ALubyte*>(buffer->data);
                            ALsizei DataSize;

                            if(LoopStart >= buffer->SampleLen)
                                continue;

                            DataSize = mini(SizeToDo, buffer->SampleLen - LoopStart);
                            CompLen = maxi(CompLen, DataSize);

                            LoadSamples(&SrcData[FilledAmt],
                                &Data[(LoopStart*NumChannels + chan)*SampleSize],
                                NumChannels, buffer->FmtType, DataSize
                            );
                        }
                        FilledAmt += CompLen;
                    }
                }
            }
            else
            {
                /* Crawl the buffer queue to fill in the temp buffer */
                ALbufferlistitem *tmpiter = BufferListItem;
                ALsizei pos = DataPosInt;

                while(tmpiter && SrcBufferSize > FilledAmt)
                {
                    ALsizei SizeToDo = SrcBufferSize - FilledAmt;
                    ALsizei CompLen = 0;
                    ALsizei i;

                    for(i = 0;i < tmpiter->num_buffers;i++)
                    {
                        const ALbuffer *ALBuffer = tmpiter->buffers[i];
                        ALsizei DataSize = ALBuffer ? ALBuffer->SampleLen : 0;

                        if(DataSize > pos)
                        {
                            const ALubyte *Data = static_cast<const ALubyte*>(ALBuffer->data);
                            Data += (pos*NumChannels + chan)*SampleSize;

                            DataSize = mini(SizeToDo, DataSize - pos);
                            CompLen = maxi(CompLen, DataSize);

                            LoadSamples(&SrcData[FilledAmt], Data, NumChannels,
                                        ALBuffer->FmtType, DataSize);
                        }
                    }
                    if(UNLIKELY(!CompLen))
                        pos -= tmpiter->max_samples;
                    else
                    {
                        FilledAmt += CompLen;
                        if(SrcBufferSize <= FilledAmt)
                            break;
                        pos = 0;
                    }
                    tmpiter = ATOMIC_LOAD(&tmpiter->next, almemory_order_acquire);
                    if(!tmpiter) tmpiter = BufferLoopItem;
                }
            }

            /* Store the last source samples used for next time. */
            memcpy(voice->PrevSamples[chan],
                &SrcData[(increment*DstBufferSize + DataPosFrac)>>FRACTIONBITS],
                MAX_RESAMPLE_PADDING*sizeof(ALfloat)
            );

            /* Now resample, then filter and mix to the appropriate outputs. */
            ResampledData = Resample(&voice->ResampleState,
                &SrcData[MAX_RESAMPLE_PADDING], DataPosFrac, increment,
                Device->TempBuffer[RESAMPLED_BUF], DstBufferSize
            );
            {
                DirectParams *parms = &voice->Direct.Params[chan];
                const ALfloat *samples;

                samples = DoFilters(
                    &parms->LowPass, &parms->HighPass, Device->TempBuffer[FILTERED_BUF],
                    ResampledData, DstBufferSize, voice->Direct.FilterType
                );
                if(!(voice->Flags&VOICE_HAS_HRTF))
                {
                    if(!Counter)
                        memcpy(parms->Gains.Current, parms->Gains.Target,
                               sizeof(parms->Gains.Current));
                    if(!(voice->Flags&VOICE_HAS_NFC))
                        MixSamples(samples, voice->Direct.Channels, voice->Direct.Buffer,
                            parms->Gains.Current, parms->Gains.Target, Counter, OutPos,
                            DstBufferSize
                        );
                    else
                    {
                        ALfloat *nfcsamples = Device->TempBuffer[NFC_DATA_BUF];
                        ALsizei chanoffset = 0;

                        MixSamples(samples,
                            voice->Direct.ChannelsPerOrder[0], voice->Direct.Buffer,
                            parms->Gains.Current, parms->Gains.Target, Counter, OutPos,
                            DstBufferSize
                        );
                        chanoffset += voice->Direct.ChannelsPerOrder[0];
#define APPLY_NFC_MIX(order)                                                  \
    if(voice->Direct.ChannelsPerOrder[order] > 0)                             \
    {                                                                         \
        NfcFilterProcess##order(&parms->NFCtrlFilter, nfcsamples, samples,    \
                               DstBufferSize);                                \
        MixSamples(nfcsamples, voice->Direct.ChannelsPerOrder[order],         \
            voice->Direct.Buffer+chanoffset, parms->Gains.Current+chanoffset, \
            parms->Gains.Target+chanoffset, Counter, OutPos, DstBufferSize    \
        );                                                                    \
        chanoffset += voice->Direct.ChannelsPerOrder[order];                  \
    }
                        APPLY_NFC_MIX(1)
                        APPLY_NFC_MIX(2)
                        APPLY_NFC_MIX(3)
#undef APPLY_NFC_MIX
                    }
                }
                else
                {
                    MixHrtfParams hrtfparams;
                    ALsizei fademix = 0;
                    int lidx, ridx;

                    lidx = GetChannelIdxByName(&Device->RealOut, FrontLeft);
                    ridx = GetChannelIdxByName(&Device->RealOut, FrontRight);
                    assert(lidx != -1 && ridx != -1);

                    if(!Counter)
                    {
                        /* No fading, just overwrite the old HRTF params. */
                        parms->Hrtf.Old = parms->Hrtf.Target;
                    }
                    else if(!(parms->Hrtf.Old.Gain > GAIN_SILENCE_THRESHOLD))
                    {
                        /* The old HRTF params are silent, so overwrite the old
                         * coefficients with the new, and reset the old gain to
                         * 0. The future mix will then fade from silence.
                         */
                        parms->Hrtf.Old = parms->Hrtf.Target;
                        parms->Hrtf.Old.Gain = 0.0f;
                    }
                    else if(firstpass)
                    {
                        ALfloat gain;

                        /* Fade between the coefficients over 128 samples. */
                        fademix = mini(DstBufferSize, 128);

                        /* The new coefficients need to fade in completely
                         * since they're replacing the old ones. To keep the
                         * gain fading consistent, interpolate between the old
                         * and new target gains given how much of the fade time
                         * this mix handles.
                         */
                        gain = lerp(parms->Hrtf.Old.Gain, parms->Hrtf.Target.Gain,
                                    minf(1.0f, (ALfloat)fademix/Counter));
                        hrtfparams.Coeffs = parms->Hrtf.Target.Coeffs;
                        hrtfparams.Delay[0] = parms->Hrtf.Target.Delay[0];
                        hrtfparams.Delay[1] = parms->Hrtf.Target.Delay[1];
                        hrtfparams.Gain = 0.0f;
                        hrtfparams.GainStep = gain / (ALfloat)fademix;

                        MixHrtfBlendSamples(
                            voice->Direct.Buffer[lidx], voice->Direct.Buffer[ridx],
                            samples, voice->Offset, OutPos, IrSize, &parms->Hrtf.Old,
                            &hrtfparams, &parms->Hrtf.State, fademix
                        );
                        /* Update the old parameters with the result. */
                        parms->Hrtf.Old = parms->Hrtf.Target;
                        if(fademix < Counter)
                            parms->Hrtf.Old.Gain = hrtfparams.Gain;
                    }

                    if(fademix < DstBufferSize)
                    {
                        ALsizei todo = DstBufferSize - fademix;
                        ALfloat gain = parms->Hrtf.Target.Gain;

                        /* Interpolate the target gain if the gain fading lasts
                         * longer than this mix.
                         */
                        if(Counter > DstBufferSize)
                            gain = lerp(parms->Hrtf.Old.Gain, gain,
                                        (ALfloat)todo/(Counter-fademix));

                        hrtfparams.Coeffs = parms->Hrtf.Target.Coeffs;
                        hrtfparams.Delay[0] = parms->Hrtf.Target.Delay[0];
                        hrtfparams.Delay[1] = parms->Hrtf.Target.Delay[1];
                        hrtfparams.Gain = parms->Hrtf.Old.Gain;
                        hrtfparams.GainStep = (gain - parms->Hrtf.Old.Gain) / (ALfloat)todo;
                        MixHrtfSamples(
                            voice->Direct.Buffer[lidx], voice->Direct.Buffer[ridx],
                            samples+fademix, voice->Offset+fademix, OutPos+fademix, IrSize,
                            &hrtfparams, &parms->Hrtf.State, todo
                        );
                        /* Store the interpolated gain or the final target gain
                         * depending if the fade is done.
                         */
                        if(DstBufferSize < Counter)
                            parms->Hrtf.Old.Gain = gain;
                        else
                            parms->Hrtf.Old.Gain = parms->Hrtf.Target.Gain;
                    }
                }
            }

            for(send = 0;send < Device->NumAuxSends;send++)
            {
                SendParams *parms = &voice->Send[send].Params[chan];
                const ALfloat *samples;

                if(!voice->Send[send].Buffer)
                    continue;

                samples = DoFilters(
                    &parms->LowPass, &parms->HighPass, Device->TempBuffer[FILTERED_BUF],
                    ResampledData, DstBufferSize, voice->Send[send].FilterType
                );

                if(!Counter)
                    memcpy(parms->Gains.Current, parms->Gains.Target,
                           sizeof(parms->Gains.Current));
                MixSamples(samples, voice->Send[send].Channels, voice->Send[send].Buffer,
                    parms->Gains.Current, parms->Gains.Target, Counter, OutPos, DstBufferSize
                );
            }
        }
        /* Update positions */
        DataPosFrac += increment*DstBufferSize;
        DataPosInt  += DataPosFrac>>FRACTIONBITS;
        DataPosFrac &= FRACTIONMASK;

        OutPos += DstBufferSize;
        voice->Offset += DstBufferSize;
        Counter = maxi(DstBufferSize, Counter) - DstBufferSize;
        firstpass = false;

        if(isstatic)
        {
            if(BufferLoopItem)
            {
                /* Handle looping static source */
                const ALbuffer *Buffer = BufferListItem->buffers[0];
                ALsizei LoopStart = Buffer->LoopStart;
                ALsizei LoopEnd = Buffer->LoopEnd;
                if(DataPosInt >= LoopEnd)
                {
                    assert(LoopEnd > LoopStart);
                    DataPosInt = ((DataPosInt-LoopStart)%(LoopEnd-LoopStart)) + LoopStart;
                }
            }
            else
            {
                /* Handle non-looping static source */
                if(DataPosInt >= BufferListItem->max_samples)
                {
                    isplaying = false;
                    BufferListItem = NULL;
                    DataPosInt = 0;
                    DataPosFrac = 0;
                    break;
                }
            }
        }
        else while(1)
        {
            /* Handle streaming source */
            if(BufferListItem->max_samples > DataPosInt)
                break;

            DataPosInt -= BufferListItem->max_samples;

            buffers_done += BufferListItem->num_buffers;
            BufferListItem = ATOMIC_LOAD(&BufferListItem->next, almemory_order_relaxed);
            if(!BufferListItem && !(BufferListItem=BufferLoopItem))
            {
                isplaying = false;
                DataPosInt = 0;
                DataPosFrac = 0;
                break;
            }
        }
    } while(isplaying && OutPos < SamplesToDo);

    voice->Flags |= VOICE_IS_FADING;

    /* Update source info */
    ATOMIC_STORE(&voice->position, static_cast<ALuint>(DataPosInt), almemory_order_relaxed);
    ATOMIC_STORE(&voice->position_fraction, DataPosFrac, almemory_order_relaxed);
    ATOMIC_STORE(&voice->current_buffer,    BufferListItem, almemory_order_release);

    /* Send any events now, after the position/buffer info was updated. */
    enabledevt = ATOMIC_LOAD(&Context->EnabledEvts, almemory_order_acquire);
    if(buffers_done > 0 && (enabledevt&EventType_BufferCompleted))
        SendAsyncEvent(Context, EventType_BufferCompleted,
            AL_EVENT_TYPE_BUFFER_COMPLETED_SOFT, SourceID, buffers_done, "Buffer completed"
        );

    return isplaying;
}
