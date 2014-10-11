/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2013-2014 Igor Zinken - http://www.igorski.nl
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "synthevent.h"
#include "audioengine.h"
#include "sequencer.h"
#include "utils.h"
#include "global.h"
#include <cmath>

/* constructor / destructor */

/**
 * initializes an SynthEvent for use in a sequenced context
 *
 * @param aFrequency   {int}    the frequency of the note to synthesize in Hz
 * @param aPosition    {int}    the step position in the sequencer
 * @param aLength      {float} the length in steps the note lasts
 * @param aInstrument  {SynthInstrument} the instruments properties
 * @param aAutoCache   {bool} whether to start caching automatically, this is
 *                     only available if AudioEngineProps::EVENT_CACHING is true
 */
SynthEvent::SynthEvent( float aFrequency, int aPosition, float aLength,
                        SynthInstrument *aInstrument, bool aAutoCache )
{
    init( aInstrument, aFrequency, aPosition, aLength, false, true );
    setAutoCache( aAutoCache );
}

/**
 * initializes an SynthEvent for use in a sequenced context
 *
 * @param aFrequency   {int}    the frequency of the note to synthesize in Hz
 * @param aPosition    {int}    the step position in the sequencer
 * @param aLength      {float} the length in steps the note lasts
 * @param aInstrument  {SynthInstrument} the instruments properties
 * @param aAutoCache   {bool} whether to start caching automatically, this is
 *                     only available if AudioEngineProps::EVENT_CACHING is true
 * @param hasParent    {bool} whether to omit creation of OSC2 (blocks endless recursion)
 */
SynthEvent::SynthEvent( float aFrequency, int aPosition, float aLength,
                        SynthInstrument *aInstrument, bool aAutoCache, bool hasParent )
{
    init( aInstrument, aFrequency, aPosition, aLength, hasParent, true );
    setAutoCache( aAutoCache );
}

/**
 * initializes an SynthEvent to be synthesized at once, for a live instrument context
 *
 * @param aFrequency  {int} the frequency of the note to synthesize in Hz
 * @param aInstrument {SynthInstrument}
 */
SynthEvent::SynthEvent( float aFrequency, SynthInstrument *aInstrument )
{
    init( aInstrument, aFrequency, 0, 1, false, false );
}

/**
 * initializes an SynthEvent to be synthesized at once, for a live instrument context
 *
 * @param aFrequency  {int} the frequency of the note to synthesize in Hz
 * @param aInstrument {SynthInstrument}
 * @param hasParent    {bool} whether to omit creation of OSC2 (blocks endless recursion)
 */
SynthEvent::SynthEvent( float aFrequency, SynthInstrument *aInstrument, bool hasParent )
{
    init( aInstrument, aFrequency, 0, 1, hasParent, false );
}

SynthEvent::~SynthEvent()
{
    if ( _ringBuffer != 0 )
    {
        delete _ringBuffer;
        _ringBuffer = 0;
    }

    if ( _arpeggiator != 0 )
    {
        delete _arpeggiator;
        _arpeggiator = 0;
    }

    if ( _adsr != 0 )
    {
        delete _adsr;
        _adsr = 0;
    }

    // secondary oscillator
    destroyOSC2();

    // remove AudioEvent from sequencer

    if ( !isSequenced )
    {
        for ( int i; i < _instrument->liveEvents->size(); i++ )
        {
            if ( _instrument->liveEvents->at( i ) == this )
            {
                _instrument->liveEvents->erase( _instrument->liveEvents->begin() + i );
                break;
            }
        }
    }
    else
    {
        for ( int i; i < _instrument->audioEvents->size(); i++ )
        {
            if ( _instrument->audioEvents->at( i ) == this )
            {
                _instrument->audioEvents->erase( _instrument->audioEvents->begin() + i );
                break;
            }
        }
    }
}

/* public methods */

/**
 * will only occur for a sequenced SynthEvent
 */
void SynthEvent::mixBuffer( AudioBuffer* outputBuffer, int bufferPos, int minBufferPosition, int maxBufferPosition,
                            bool loopStarted, int loopOffset, bool useChannelRange )
{
    // is EVENT_CACHING is enabled, read from cached buffer

    if ( AudioEngineProps::EVENT_CACHING )
    {
        BaseAudioEvent::mixBuffer( outputBuffer, bufferPos, minBufferPosition, maxBufferPosition, loopStarted, loopOffset, useChannelRange );
    }
    else
    {
        int bufferEndPos = bufferPos + AudioEngineProps::BUFFER_SIZE;

        // EVENT_CACHING is disabled, synthesize on the fly
        // ex. : START 200 | END 2000 | LENGTH 1800 | CURRENT BUFFER POS 0 @ BUFFER SIZE 512
        if (( bufferPos >= _sampleStart || bufferEndPos > _sampleStart ) &&
              bufferPos < _sampleEnd )
        {
            // render the snippet
            _cacheWriteIndex = _sampleStart > bufferPos ? 0 : bufferPos - _sampleStart;
            int writeOffset  = _sampleStart > bufferPos ? _sampleStart - bufferPos : 0;

            render( _buffer ); // overwrites old buffer contents
            outputBuffer->mergeBuffers( _buffer, 0, writeOffset, MAX_PHASE );

            // reset of properties at end of write
            if ( _cacheWriteIndex >= _sampleLength )
                calculateBuffers();
        }
        // TODO : loop start seamless reading required ?
    }
}

AudioBuffer* SynthEvent::getBuffer()
{
    if ( AudioEngineProps::EVENT_CACHING )
    {
        // if caching hasn't completed, fill cache fragment
        if ( !_cachingCompleted )
            doCache();
    }
    return _buffer;
}

float SynthEvent::getFrequency()
{
    return _frequency;
}

void SynthEvent::setFrequency( float aFrequency )
{
    setFrequency( aFrequency, true, true );
}

void SynthEvent::setFrequency( float aFrequency, bool allOscillators, bool storeAsBaseFrequency )
{
    float currentFreq = _frequency;
    _frequency        = aFrequency;
    //_phase            = 0.0f; // will create nasty pop if another freq was playing previously
    _phaseIncr        = aFrequency / AudioEngineProps::SAMPLE_RATE;

    // store as base frequency (acts as a reference "return point" for pitch shifting modules)
    if ( storeAsBaseFrequency )
        _baseFrequency = aFrequency;

    if ( /*!isSequenced &&*/ _type == WaveForms::KARPLUS_STRONG )
        initKarplusStrong();

    // update properties of secondary oscillator, note that OSC2 can
    // have a pitch that deviates from the first oscillator
    // as such we multiply it by the deviation of the new frequency
    if ( allOscillators && _osc2 != 0 )
    {
        float multiplier = aFrequency / currentFreq;
        _osc2->setFrequency( _osc2->_frequency * multiplier, true, storeAsBaseFrequency );
    }
}

/**
 * @param aPosition position in the sequencer where this event starts playing
 * @param aLength length (in sequencer steps) of this event
 * @param aInstrument the SynthInstrument whose properties will be used for synthesizing this event
 * @param aState which oscillator(s) to update 0 = all, 1 = oscillator 1, 2 = oscillator 2
 *               this is currently rudimentary as both oscillators are rendered and merged into one
 *               this is here for either legacy purposes or when performance improvements can be gained
 */
void SynthEvent::updateProperties( int aPosition, float aLength, SynthInstrument *aInstrument, int aState )
{
    bool updateOSC1 = true;//( aState == 0 || aState == 1 );
    bool updateOSC2 = true;//( aState == 0 || aState == 2 );

    _type    = aInstrument->waveform;
    position = aPosition;
    length   = aLength;

    _adsr->cloneEnvelopes( aInstrument->adsr );

    // secondary oscillator

    if ( updateOSC2 && aInstrument->osc2active )
        createOSC2( aPosition, aLength, aInstrument );
    else
        destroyOSC2();

    // modules

    applyModules( aInstrument );

    if ( updateOSC1 )
    {
        if ( _caching /*&& !_cachingCompleted */)
        {
            if ( _osc2 != 0 /*&& _osc2->_caching*/ )
                _osc2->_cancel = true;

            _cancel = true;
        }
        else {
            calculateBuffers();
        }
    }
}

void SynthEvent::unlock()
{
    _locked = false;

    if ( _updateAfterUnlock )
        calculateBuffers();

    _updateAfterUnlock = false;
}

void SynthEvent::calculateBuffers()
{
    if ( _locked )
    {
        _updateAfterUnlock = true;
        return;
    }

    int oldLength;

    if ( isSequenced )
    {
        if ( _caching )
            _cancel = true;

        oldLength     = _sampleLength;
        _sampleLength = ( int )( length * ( float ) AudioEngine::bytes_per_tick );
        _sampleStart  = position * AudioEngine::bytes_per_tick;
        _sampleEnd    = _sampleStart + _sampleLength;
    }
    else {
        // quick releases of the key should at least ring for a 32nd note
        _minLength    = AudioEngine::bytes_per_bar / 32;
        _sampleLength = AudioEngine::bytes_per_bar;     // important for amplitude swell in
        oldLength     = AudioEngineProps::BUFFER_SIZE;  // buffer is as long as the engine's buffer size
        _hasMinLength = false;                          // keeping track if the min length has been rendered
    }

    _adsr->setBufferLength( _sampleLength );

    // sample length changed (f.i. tempo change) or buffer not yet created ?
    // create buffer for (new) sample length
    if ( _sampleLength != oldLength || _buffer == 0 )
    {
        destroyBuffer(); // clear previous buffer contents

        // OSC2 generates no buffer (writes into parent buffer, saves memory)
        if ( !hasParent )
        {
            // note that when event caching is enabled, the buffer is as large as
            // the total event length requires

            if ( AudioEngineProps::EVENT_CACHING && isSequenced )
                _buffer = new AudioBuffer( AudioEngineProps::OUTPUT_CHANNELS, _sampleLength );
            else
                _buffer = new AudioBuffer( AudioEngineProps::OUTPUT_CHANNELS, AudioEngineProps::BUFFER_SIZE );
        }
     }

    if ( isSequenced )
    {
        if ( _type == WaveForms::KARPLUS_STRONG )
            initKarplusStrong();

        if ( AudioEngineProps::EVENT_CACHING )
        {
            resetCache(); // yes here, not in cache()-invocation as cancels might otherwise remain permanent (see BulkCacher)

            // (re)cache (unless this event is OSC2 as only the parent event can invoke the render)
            if ( _autoCache && !hasParent )
            {
                if ( !_caching )
                    cache( false );
                else
                    _cancel = true;
            }
        }
    }
}

/**
 * synthesize is invoked by the Sequencer for rendering a non-sequenced
 * SynthEvent into a single buffer
 *
 * aBufferLength {int} length of the buffer to synthesize
 */
AudioBuffer* SynthEvent::synthesize( int aBufferLength )
{
    if ( aBufferLength != AudioEngineProps::BUFFER_SIZE )
    {
        // clear previous buffer contents
        destroyBuffer();
        _buffer = new AudioBuffer( AudioEngineProps::OUTPUT_CHANNELS, aBufferLength );
    }
    render( _buffer ); // overwrites old buffer contents

    // keep track of the rendered bytes, in case of a key up event
    // we still want to have the sound ring for the minimum period
    // defined in the constructor instead of cut off immediately

    if ( _queuedForDeletion && _minLength > 0 )
        _minLength -= aBufferLength;

    if ( _minLength <= 0 )
    {
        _hasMinLength = true;
        setDeletable( _queuedForDeletion );

        // event is about to be deleted, apply a tiny fadeout
        if ( _queuedForDeletion )
        {
            int amt = ceil( aBufferLength / 4 );

            float envIncr = MAX_PHASE / amt;
            float amp     = MAX_PHASE;

            for ( int i = aBufferLength - amt; i < aBufferLength; ++i )
            {
                for ( int c = 0, nc = _buffer->amountOfChannels; c < nc; ++c )
                    _buffer->getBufferForChannel( c )[ i ] *= amp;

                amp -= envIncr;
            }
        }
    }
    return _buffer;
}

/**
 * (pre-)cache the contents of the SynthEvent in its entirety
 * this can be done in idle time to make optimum use of resources
 */
void SynthEvent::cache( bool doCallback )
{
    if ( _buffer == 0 ) // this cache request was invoked after destruction
        return;

    _caching = true;

    doCache();

    if ( doCallback )
        sequencer::bulkCacher->cacheQueue();
}

ADSR* SynthEvent::getADSR()
{
    return _adsr;
}

float SynthEvent::getVolume()
{
    return _volume;
}

void SynthEvent::setVolume( float aValue )
{
    _volume = aValue;
}

/* private methods */

void SynthEvent::initKarplusStrong()
{
    // reset previous _ringBuffer data
    int prevRingBufferSize = _ringBufferSize;
    _ringBufferSize        = ( int ) ( AudioEngineProps::SAMPLE_RATE / _frequency );
    bool newSize           = _ringBufferSize != prevRingBufferSize;

    if ( isSequenced && ( _ringBuffer != 0 && newSize ))
    {
        delete _ringBuffer;
        _ringBuffer = 0;
    }

    if ( _ringBuffer == 0 )
        _ringBuffer = new RingBuffer( _ringBufferSize );
    else
        _ringBuffer->flush();

    // fill the ring buffer with noise ( initial "pluck" of the "string" )
    for ( int i = 0; i < _ringBufferSize; i++ )
        _ringBuffer->enqueue( randomFloat());
}

/**
 * the actual synthesizing of the audio
 *
 * @param aOutputBuffer {AudioBuffer*} the buffer to write into
 */
void SynthEvent::render( AudioBuffer* aOutputBuffer )
{
    int i;
    int bufferLength = aOutputBuffer->bufferSize;

    SAMPLE_TYPE amp = 0.0;
    SAMPLE_TYPE tmp, am, dpw, pmv;

    bool hasOSC2 = _osc2 != 0;

    int renderStartOffset = AudioEngineProps::EVENT_CACHING && isSequenced ? _cacheWriteIndex : 0;

    int maxSampleIndex  = _sampleLength - 1;                // max index possible for this events length
    int renderEndOffset = renderStartOffset + bufferLength; // max buffer index to be written to in this cycle

    // keep in bounds of event duration
    if ( renderEndOffset > maxSampleIndex )
    {
        renderEndOffset = maxSampleIndex;
        aOutputBuffer->silenceBuffers(); // as we tend to overwrite the incoming buffer
    }

    for ( i = renderStartOffset; i < renderEndOffset; ++i )
    {
        switch ( _type )
        {
            case WaveForms::SINE_WAVE:

                // ---- Sine wave

                if ( _phase < .5 )
                {
                    tmp = ( _phase * 4.0 - 1.0 );
                    amp = ( 1.0 - tmp * tmp );
                }
                else {
                    tmp = ( _phase * 4.0 - 3.0 );
                    amp = ( tmp * tmp - 1.0 );
                }
                amp *= .7;  // sines distorted easily when overlapping

                break;

            case WaveForms::SAWTOOTH:

                // ---- Sawtooth
                amp = ( _phase < 0 ) ? _phase - ( int )( _phase - 1 ) : _phase - ( int )( _phase );

                break;

            case WaveForms::SQUARE_WAVE:

                // ---- Square wave
                if ( _phase < .5 )
                {
                    tmp = TWO_PI * ( _phase * 4.0 - 1.0 );
                    amp = ( 1.0 - tmp * tmp );
                }
                else {
                    tmp = TWO_PI * ( _phase * 4.0 - 3.0 );
                    amp = ( tmp * tmp - 1.0 );
                }
                amp *= .01; // these get loud ! 0.005
                break;

            case WaveForms::TRIANGLE:

                // ---- triangle
                if ( _phase < .5 )
                {
                    tmp = ( _phase * 4.0 - 1.0 );
                    amp = ( 1.0 - tmp * tmp ) * .75;
                }
                else {
                    tmp = ( _phase * 4.0 - 3.0 );
                    amp = ( tmp * tmp - 1.0 ) * .75;
                }
                // the actual triangulation function
                amp = amp < 0 ? -amp : amp;

                break;

            case WaveForms::PWM:

                // --- pulse width modulation
                pmv = i + ( ++_pwmValue ); // i + event position

                dpw = sin( pmv / 0x4800 ) * pwr; // LFO -> PW
                amp = _phase < PI - dpw ? pwAmp : -pwAmp;

                // PWM has its own phase update operation
                _phase = _phase + ( TWO_PI_OVER_SR * _frequency );
                _phase = _phase > TWO_PI ? _phase - TWO_PI : _phase;

                // we multiply the amplitude as PWM results in a "quieter" wave
                amp *= 4;

                /* // OLD: oscillation modulating the PW wave
                am   = sin( pmv / 0x1000 ); // LFO -> AM
                amp *= am;
                */
                break;

            case WaveForms::NOISE:

                // --- noise
                if ( _phase < .5 )
                {
                    tmp = ( _phase * 4.0 - 1.0 );
                    amp = ( 1.0 - tmp * tmp );
                }
                else {
                    tmp = ( _phase * 4.0 - 3.0 );
                    amp = ( tmp * tmp - 1.0 );
                }
                // above we calculated pitch, now we add some
                // randomization to the signal for the actual noise
                amp *= randomFloat();
                break;

            case WaveForms::KARPLUS_STRONG:

                // --- Karplus-Strong algorithm for plucked string-sound
                _ringBuffer->enqueue(( EnergyDecayFactor * (( _ringBuffer->dequeue() + _ringBuffer->peek()) / 2 )));
                amp = _ringBuffer->peek(); // * .7; gets the level down a bit, 'tis loud

                break;
        }

        // --- _phase update operations
        if ( _type != WaveForms::PWM )
        {
            _phase += _phaseIncr;

            // restore _phase, max range is 0 - 1 ( float )
            if ( _phase > MAX_PHASE )
                _phase -= MAX_PHASE;
        }

        // update modules
        if ( _arpeggiator != 0 )
        {
            // step the arpeggiator to the next position
            if ( _arpeggiator->peek())
                setFrequency( _arpeggiator->getPitchForStep( _arpeggiator->getStep(), _baseFrequency ), true, false );
        }

        // -- write the output into the buffers channels
        if ( hasOSC2 ) amp *= .5;

        for ( int c = 0, ca = aOutputBuffer->amountOfChannels; c < ca; ++c )
            aOutputBuffer->getBufferForChannel( c )[ i ] = amp * _volume;

        // stop caching/rendering loop when cancel was requested
        if ( _cancel )
            break;
    }

    // secondary oscillator ? render its contents into this (parent) buffer

    if ( hasOSC2 && !_cancel )
    {
        // create a temporary buffer (this prevents writing to deleted buffers
        // when the parent event changes its _buffer properties (f.i. tempo change)
        int tempLength = ( AudioEngineProps::EVENT_CACHING && isSequenced ) ? ( renderEndOffset - _cacheWriteIndex ) : bufferLength;
        AudioBuffer* tempBuffer = new AudioBuffer( aOutputBuffer->amountOfChannels, tempLength );
        _osc2->render( tempBuffer );
        aOutputBuffer->mergeBuffers( tempBuffer, 0, renderStartOffset, MAX_PHASE );

        delete tempBuffer; // free allocated memory
    }

    // apply envelopes and update cacheWriteIndex for next render cycle
    if ( !hasParent )
    {
        _adsr->apply( aOutputBuffer, _cacheWriteIndex );
        _cacheWriteIndex += i;
    }

    if ( AudioEngineProps::EVENT_CACHING )
    {
        if ( isSequenced )
        {
            _caching = false;

            // was a cancel requested ? re-cache to match the new instrument properties (cancel
            // may only be requested during the changing of properties!)
            if ( _cancel )
            {
                calculateBuffers();
            }
            else
            {
                if ( i == maxSampleIndex )
                    _cachingCompleted = true;

                if ( _bulkCacheable )
                    _autoCache = true;
            }
        }
    }
    _cancel = false; // ensure we can render for the next iteration
}

void SynthEvent::setDeletable( bool value )
{
    // pre buffered event or rendered min length ? schedule for immediate deletion

    if ( isSequenced || _hasMinLength )
        _deleteMe = value;
    else
        _queuedForDeletion = value;

    // secondary oscillator too

    if ( _osc2 != 0 )
        _osc2->setDeletable( value );
}

/**
 * @param aInstrument pointer to the SynthInstrument containing the rendering properties for the SynthEvent
 * @param aFrequency  frequency in Hz for the note to be rendered
 * @param aPosition   offset in the sequencer where this event starts playing / becomes audible
 * @param aLength     length of the event (in sequencer steps)
 * @param aHasParent  when true, this SynthEvent will be merged into the buffer of its parent instead of being
 *                    added to the Sequencer as an individual event, this makes rendering of its parent event
 *                    draw more CPU as its rendering multiple buffers, but after merging it consumes less memory
 *                    than two individual buffers would, it also omits the need of having float SynthEvents
 *                    to be mixed by the Sequencer
 * @param aIsSequenced whether this event is sequenced and only audible in a specific sequence range
 */
void SynthEvent::init( SynthInstrument *aInstrument, float aFrequency, int aPosition,
                       int aLength, bool aHasParent, bool aIsSequenced )
{
    _destroyableBuffer = true;  // always unique and managed by this instance !
    _instrument        = aInstrument;
    _adsr              = _instrument->adsr->clone();

    // when instrument has no fixed length and the decay is short
    // we deactivate the decay envelope completely (for now)
    if ( !aIsSequenced && _adsr->getDecay() < .75 )
        _adsr->setDecay( 0 );

    _buffer         = 0;
    _ringBuffer     = 0;
    _ringBufferSize = 0;
    _locked         = false;
    _frequency      = aFrequency;
    _baseFrequency  = aFrequency;

    position        = aPosition;
    length          = aLength;
    hasParent       = aHasParent;

    isSequenced            = aIsSequenced;
    _queuedForDeletion     = false;
    _deleteMe              = false;
    _cancel                = false; // whether we should cancel caching
    _caching               = false;
    _cachingCompleted      = false; // whether we're done caching
    _autoCache             = false; // we'll cache sequentially instead
    _type                  = aInstrument->waveform;
    _osc2                  = 0;
    _volume                = aInstrument->volume;
    _sampleLength          = 0;
    _cacheWriteIndex       = 0;

    // constants used by waveform generators

    TWO_PI_OVER_SR         = TWO_PI / AudioEngineProps::SAMPLE_RATE;
    pwr                    = PI / 1.05;
    pwAmp                  = 0.075;
    EnergyDecayFactor      = 0.990f; // TODO make this settable ?
    _pwmValue              = 0.0;
    _phase                 = 0.0;

    // secondary oscillator, note different constructor
    // to omit going into recursion!

    if ( !hasParent && aInstrument->osc2active )
       createOSC2( position, length, aInstrument );

    setFrequency( aFrequency );

    // modules

    _arpeggiator = 0;
    applyModules( aInstrument );

    // buffer

    _hasMinLength = isSequenced; // a sequenced event has no early cancel
    calculateBuffers();

    // add the event to the sequencer so it can be heard
    // note that OSC2 contents aren't added to the sequencer
    // individually as their render is invoked by their parent,
    // writing directly into their parent buffer (saves memory overhead)

    if ( isSequenced )
    {
        if ( !hasParent )
            aInstrument->audioEvents->push_back( this );
    }
    else {
        if ( !hasParent )
            aInstrument->liveEvents->push_back( this );
    }
}

/**
 * creates a new/updates an existing secondary oscillator
 *
 * @param aPosition {int} sequencer position where the event starts playing
 * @param aLength   {int} duration (in sequencer steps) the event keeps playing
 * @param aInstrument {SynthInstrument} the synth instrument whose properties are used for synthesis
 */
void SynthEvent::createOSC2( int aPosition, int aLength, SynthInstrument *aInstrument )
{
    if ( aInstrument->osc2active )
    {
        // note no auto caching for a sequenced OSC2, its render is invoked by its parent (=this) event!
        if ( _osc2 == 0 )
        {
            if ( !isSequenced )
                _osc2 = new SynthEvent( _frequency, aInstrument, true );
            else
                _osc2 = new SynthEvent( _frequency, aPosition, aLength, aInstrument, false, true );
        }
        // seems verbose, but in case of updating an existing OSC2, necessary
        _osc2->_type    = aInstrument->osc2waveform;
        _osc2->position = aPosition;
        _osc2->length   = aLength;

        float lfo2Tmpfreq = _frequency + ( _frequency / 1200 * aInstrument->osc2detune ); // 1200 cents == octave
        float lfo2freq    = lfo2Tmpfreq;

        // octave shift ( -2 to +2 )
        if ( aInstrument->osc2octaveShift != 0 )
        {
            if ( aInstrument->osc2octaveShift < 0 )
                lfo2freq = lfo2Tmpfreq / std::abs(( float ) ( aInstrument->osc2octaveShift * 2 ));
            else
                lfo2freq += ( lfo2Tmpfreq * std::abs(( float ) ( aInstrument->osc2octaveShift * 2 ) - 1 ));
        }
        // fine shift ( -7 to +7 )
        float fineShift = ( lfo2Tmpfreq / 12 * std::abs( aInstrument->osc2fineShift ));

        if ( aInstrument->osc2fineShift < 0 )
            lfo2freq -= fineShift;
         else
            lfo2freq += fineShift;

        _osc2->setFrequency( lfo2freq );

        if ( _osc2->_caching /*&& !_osc2->_cachingCompleted */)
            _osc2->_cancel = true;
    }
}

void SynthEvent::destroyOSC2()
{
    if ( _osc2 != 0 )
    {
        if ( _osc2->_caching /*&& !_osc2->_cachingCompleted */)
            _osc2->_cancel = true;

        delete _osc2;
        _osc2 = 0;
    }
}

void SynthEvent::applyModules( SynthInstrument* instrument )
{
    bool hasOSC2   = _osc2 != 0;
    float OSC2freq = hasOSC2 ? _osc2->_baseFrequency : _baseFrequency;

    if ( _arpeggiator != 0 )
    {
        delete _arpeggiator;
        _arpeggiator = 0;
    }

    if ( instrument->arpeggiatorActive )
        _arpeggiator = instrument->arpeggiator->clone();

    if ( hasOSC2 )
        _osc2->applyModules( instrument );

    // pitch shift module active ? make sure current frequency
    // matches the current arpeggiator step
    if ( instrument->arpeggiatorActive )
    {
        setFrequency( _arpeggiator->getPitchForStep( _arpeggiator->getStep(), _baseFrequency ), true, false );
    }
    else
    {
        // restore base frequency upon deactivation of pitch shift modules
        setFrequency( _baseFrequency, false, true );

        if ( hasOSC2 )
            _osc2->setFrequency( OSC2freq, false, true );
    }
}

/**
 * render the event into the buffer and cache its
 * contents for the given bufferLength (we can cache
 * buffer fragments on demand, or all at once)
 */
void SynthEvent::doCache()
{
    render( _buffer );
}

void SynthEvent::resetCache()
{
    BaseCacheableAudioEvent::resetCache();

    if ( _osc2 != 0 )
        _osc2->resetCache();
}
