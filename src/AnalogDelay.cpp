/*
 * Company: Blackaddr Audio
 * Effect Name: Analog Delay
 * Description: A warm analog-based delay with multiple filters for different flavours. Boss DM3 inspired.
 *
 * This file was auto-generated by Aviate Audio Effect Creator for the Multiverse.
 */
#include <cmath> // used for std::round
#include "Aviate/EfxPrint.h"
#include "Aviate/LibBasicFunctions.h"
#include "AnalogDelay.h"
#include "AnalogDelayFilters.h"

using namespace baCore;
using namespace Aviate;

namespace BlackaddrAudio_AnalogDelay {

AnalogDelay::AnalogDelay(float maxDelayMs, bool useExtMem)
: AudioStream(NUM_INPUTS, m_inputQueueArray)
{
    m_maxDelaySamples = calcAudioSamples(maxDelayMs);
    m_constructFilter();

    m_useExternalMemory = useExtMem;

    if (!m_useExternalMemory) {
        m_memory = new AudioDelay(maxDelayMs);
    }
}

AnalogDelay::~AnalogDelay()
{
    if (m_memory) delete m_memory;
    if (m_iir) delete m_iir;
}

// This function just sets up the default filter and coefficients
void AnalogDelay::m_constructFilter(void)
{
    // Use DM3 coefficients by default
    m_iir = new IirBiQuadFilterHQ(DM3_NUM_STAGES, reinterpret_cast<const int32_t *>(&DM3), DM3_COEFF_SHIFT);
}

void AnalogDelay::setFilterCoeffs(int numStages, const int32_t *coeffs, int coeffShift)
{
    m_iir->changeFilterCoeffs(numStages, coeffs, coeffShift);
}

void AnalogDelay::setFilter(Filter filter)
{
    EFX_PRINT(Serial.printf("AnalogDelay::setFilter(): filter set to %d\n\r", (int)filter));
    switch(filter) {
    case Filter::WARM :
        m_iir->changeFilterCoeffs(WARM_NUM_STAGES, reinterpret_cast<const int32_t *>(&WARM), WARM_COEFF_SHIFT);
        break;
    case Filter::DARK :
        m_iir->changeFilterCoeffs(DARK_NUM_STAGES, reinterpret_cast<const int32_t *>(&DARK), DARK_COEFF_SHIFT);
        break;
    case Filter::DM3 :
    default:
        m_iir->changeFilterCoeffs(DM3_NUM_STAGES, reinterpret_cast<const int32_t *>(&DM3), DM3_COEFF_SHIFT);
        break;
    }
}

void AnalogDelay::update(void)
{
    audio_block_t *inputAudioBlock = receiveReadOnly(); // get the next block of input samples

    // the following gate will not be passed until the memory is both configured
    // and cleared
    if (m_useExternalMemory && (!m_extMemConfigured || !m_extMemIsCleared) ) {
        // We can't enable this module until the SRAM Controller is available
        if (isSramReady() && !m_extMemConfigured) {
            m_configExtMem();
        }

        if (m_extMemConfigured && !m_extMemIsCleared) {
            m_clearExtMemory();
        }

        if (inputAudioBlock) {
            if (m_enable) {
                transmit(inputAudioBlock);
            }
            release(inputAudioBlock);
        }
        return;
    }

    // Check is block is disabled
    if (m_enable == false) {
        // do not transmit or process any audio, return as quickly as possible.
        if (inputAudioBlock) { release(inputAudioBlock); inputAudioBlock = nullptr; }

        // release all held memory resources
        if (m_previousBlock) {
            release(m_previousBlock);
            m_previousBlock = nullptr;
        }
        if (!m_useExternalMemory && m_memory) {
            // when using internal memory we have to release all references in the ring buffer
            while (m_memory->getRingBuffer()->size() > 0) {
                audio_block_t *releaseBlock = m_memory->getRingBuffer()->front();
                m_memory->getRingBuffer()->pop_front();
                if (releaseBlock) { release(releaseBlock); }
            }
        }
        return;
    }

    // Check is block is bypassed, if so either transmit input directly or create silence
    if ((m_bypass == true) || !(inputAudioBlock)) {

        // transmit the input directly
        if (!inputAudioBlock) {
            // create silence
            inputAudioBlock = allocate();
            if (!inputAudioBlock) { return; } // failed to allocate
            else {
                clearAudioBlock(inputAudioBlock);
            }
        }

        transmit(inputAudioBlock, 0);
        release(inputAudioBlock);
        return;
    }

    // Update the peak value
    m_updateInputPeak(inputAudioBlock);

    // Otherwise perform normal processing
    // In order to make use of the SPI DMA, we need to request the read from memory first,
    // then do other processing while it fills in the back.
    audio_block_t *blockToOutput = nullptr; // this will hold the output audio
    blockToOutput = allocate();
    if (!blockToOutput) {
        transmit(inputAudioBlock);
        release(inputAudioBlock);
        return; // skip this update cycle due to failure
    }

    // get the data. If using external memory with DMA, this won't be filled until
    // later.
    m_memory->getSamples(blockToOutput, m_delaySamples);

    // If using DMA, we need something else to do while that read executes, so
    // move on to input preprocessing

    // Preprocessing
    audio_block_t *preProcessed = allocate();
    // mix the input with the feedback path in the pre-processing stage
    m_preProcessing(preProcessed, inputAudioBlock, m_previousBlock);

    // consider doing the BBD post processing here to use up more time while waiting
    // for the read data to come back
    audio_block_t *blockToRelease = nullptr;
    blockToRelease = m_memory->addBlock(preProcessed);


    // BACK TO OUTPUT PROCESSING
    // Check if external DMA, if so, we need to be sure the read is completed
    if (m_useExternalMemory && m_memory->getSlot()->isUseDma()) {
        // Using DMA
        while (m_memory->getSlot()->isReadBusy()) {}
    }

    // perform the wet/dry mix mix
    m_postProcessing(blockToOutput, inputAudioBlock, blockToOutput);
    m_updateOutputPeak(blockToOutput);
    transmit(blockToOutput);

    release(inputAudioBlock);
    if (m_previousBlock) { release(m_previousBlock); }
    m_previousBlock = blockToOutput;

    if (m_blockToRelease) { release(m_blockToRelease); }
    m_blockToRelease = blockToRelease;
}

void AnalogDelay::m_preProcessing(audio_block_t *out, audio_block_t *dry, audio_block_t *wet)
{
    if ( out && dry && wet) {
        gainAdjust(out, wet, m_feedback, 2);
        combine(out, dry, out);
        m_iir->process(out->data, out->data, AUDIO_BLOCK_SAMPLES);
    } else if (dry) {
        memcpy(out->data, dry->data, sizeof(int16_t) * AUDIO_BLOCK_SAMPLES);
    }
}

void AnalogDelay::m_postProcessing(audio_block_t *out, audio_block_t *dry, audio_block_t *wet)
{
    if (!out) return; // no valid output buffer

    if ( out && dry && wet) {
        // Simulate the LPF IIR nature of the analog systems
        alphaBlend(out, dry, wet, m_mix);
    } else if (dry) {
        memcpy(out->data, dry->data, sizeof(int16_t) * AUDIO_BLOCK_SAMPLES);
    }
    // Set the output volume
    gainAdjust(out, out, m_volume/4.0f, 2);

}

void AnalogDelay::m_configExtMem()
{
    if (!m_extMemConfigured) {
        // the delay cannot be exactly equal to the slot size, you need at least one sample so the wr/rd are not on top of each other.
        m_slot = getSramManager()->requestMemory((m_maxDelaySamples + AUDIO_BLOCK_SAMPLES) * sizeof(int16_t), false);  // false-> don't clear memory
        if (!m_slot) {
            EFX_PRINT(Serial.println("AnalogDelay::m_configExtMem(): ERROR creating memory slot"));
            return;
        }

        m_memory = new AudioDelay(m_slot);
        if (!m_memory) {
            m_slot = nullptr;
            return;
        }
        m_slot->setWritePosition(0);
        m_extMemConfigured = true;
    }
}

void AnalogDelay::m_clearExtMemory()
{
    static size_t memoryBytesToClear = (m_maxDelaySamples + AUDIO_BLOCK_SAMPLES) * sizeof(int16_t);
    static const unsigned NUM_BYTES_PER_WRITE = (4*AUDIO_SAMPLES_PER_BLOCK*sizeof(int16_t));

    if (memoryBytesToClear > 0) {
        size_t numBytes;
        if (memoryBytesToClear > NUM_BYTES_PER_WRITE) { numBytes = NUM_BYTES_PER_WRITE;}
        else { numBytes = memoryBytesToClear; }
        // we will clear only a few blocks of memory at time in order to prevent delays in the audio service pipelin
        m_slot-> zeroAdvance(numBytes);
        memoryBytesToClear -= numBytes;
    }

    if (memoryBytesToClear == 0) {
        m_extMemIsCleared = true;
    }
}

void AnalogDelay::delayMs(float milliseconds)
{
    size_t delaySamples = calcAudioSamples(milliseconds);

    if (!m_memory) { EFX_PRINT(Serial.println("delayMs(): m_memory is not valid")); return; }

    if (!m_useExternalMemory) {
        // internal memory
        m_maxDelaySamples = m_memory->getMaxDelaySamples();
        //QueuePosition queuePosition = calcQueuePosition(milliseconds);
        //Serial.println(String("CONFIG: delay:") + delaySamples + String(" queue position ") + queuePosition.index + String(":") + queuePosition.offset);
    } else {
        // external memory
        SramMemSlot *slot = m_memory->getSlot();
        if (!slot) { Serial.println("ERROR: slot ptr is not valid");  return;}

        m_maxDelaySamples = (slot->size() / sizeof(int16_t))-AUDIO_BLOCK_SAMPLES;
    }

    if (delaySamples > m_maxDelaySamples) {
        // this exceeds max delay value, limit it.
        delaySamples = m_maxDelaySamples;
    }
    m_delaySamples = delaySamples;
}

void AnalogDelay::delaySamples(size_t numDelaySamples)
{
    if (!m_memory) { EFX_PRINT(Serial.println("delaySamples(): m_memory is not valid")); return; }

    if (!m_useExternalMemory) {
        // internal memory
        m_maxDelaySamples = m_memory->getMaxDelaySamples();
        //QueuePosition queuePosition = calcQueuePosition(delaySamples);
        //Serial.println(String("CONFIG: delay:") + delaySamples + String(" queue position ") + queuePosition.index + String(":") + queuePosition.offset);
    } else {
        // external memory
        //Serial.println(String("CONFIG: delay:") + delaySamples);
        SramMemSlot *slot = m_memory->getSlot();
        if (!slot) { return; }

        m_maxDelaySamples = (slot->size() / sizeof(int16_t))-AUDIO_BLOCK_SAMPLES;
    }

    if (numDelaySamples > m_maxDelaySamples) {
        // this exceeds max delay value, limit it.
        numDelaySamples = m_maxDelaySamples;
    }

    EFX_PRINT(Serial.printf("AnalogDelay::delaySamples(): delay samples set to %d\n\r", m_delaySamples));
    m_delaySamples = numDelaySamples;
}

void AnalogDelay::delayFractionMax(float delayFraction)
{
    if (!m_memory) { EFX_PRINT(Serial.println("delayFractionMax(): m_memory is not valid")); return; }

    size_t delaySamples;
    if (m_longdelay) {  // not 0.0, so enable long delay
        delaySamples = static_cast<size_t>(static_cast<float>(m_memory->getMaxDelaySamples()) * delayFraction);
    } else {  // 1/10th of max
        delaySamples = static_cast<size_t>(static_cast<float>(m_memory->getMaxDelaySamples()) * delayFraction * 0.1f);
    }

    //EFX_PRINT(Serial.printf("delay is %f\n\r", delayFraction); Serial.flush());

    if (!m_useExternalMemory) {
        // internal memory
        m_maxDelaySamples = m_memory->getMaxDelaySamples();
        QueuePosition queuePosition = calcQueuePosition(delaySamples);
        (void)queuePosition;  // suppress warning in Release build
        EFX_PRINT(Serial.println(String("Internal delay samples:") + delaySamples + String(" queue position ") + queuePosition.index + String(":") + queuePosition.offset));
    } else {
        // external memory
        EFX_PRINT(Serial.println(String("External delay samples:") + delaySamples));
        SramMemSlot *slot = m_memory->getSlot();
        if (!slot) { return; }

        m_maxDelaySamples = (slot->size() / sizeof(int16_t))-AUDIO_BLOCK_SAMPLES;
    }

    if (delaySamples > m_maxDelaySamples) {
        // this exceeds max delay value, limit it.
        delaySamples = m_maxDelaySamples;
    }
    m_delaySamples = delaySamples;
    EFX_PRINT(Serial.printf("AnalogDelay::delayFractionMax(): delay samples set to %d\n\r", m_delaySamples));
}


void AnalogDelay::filter(float value)
{
    // perform any necessary conversion to user variables, validation, etc.
    EFX_PRINT(Serial.printf("AnalogDelay::filter(): value is %f\n\r", value));
    m_filter = std::roundf(value * static_cast<float>(static_cast<unsigned>(Filter::NUM_FILTERS)-1));
    setFilter(static_cast<Filter>(m_filter));
}

void AnalogDelay::delay(float value)
{
    // perform any necessary conversion to user variables, validation, etc.
    m_delay = value;
    delayFractionMax(m_delay);
}

void AnalogDelay::mix(float value)
{
    // perform any necessary conversion to user variables, validation, etc.
    m_mix = value;
}

void AnalogDelay::feedback(float value)
{
    // perform any necessary conversion to user variables, validation, etc.
    constexpr float DM3_MAX_FEEDBACK_F  = 1.0f;
    constexpr float WARM_FEEDBACK_F     = 2.0f;
    constexpr float DARK_MAX_FEEDBACK_F = 3.0f;
    m_feedback = value;

    switch(static_cast<Filter>(m_filter)) {
    case Filter::WARM : m_feedback *= WARM_FEEDBACK_F;     break;
    case Filter::DARK : m_feedback *= DARK_MAX_FEEDBACK_F; break;
    case Filter::DM3  :
    default           : m_feedback *= DM3_MAX_FEEDBACK_F;  break;
    }

    m_feedback = m_feedback / 4.0f;
}

void AnalogDelay::longdelay(float value)
{
    // perform any necessary conversion to user variables, validation, etc.
    m_longdelay = value;
    delayFractionMax(m_delay);
}

void AnalogDelay::volume(float value)
{
    // perform any necessary conversion to user variables, validation, etc.
    volumeDb(-40.0f + (value * 50.0f)); // 50db dynamic range
}


}
