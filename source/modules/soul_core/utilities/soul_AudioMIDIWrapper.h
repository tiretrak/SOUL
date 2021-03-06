
/*
    _____ _____ _____ __
   |   __|     |  |  |  |      The SOUL language
   |__   |  |  |  |  |  |__    Copyright (c) 2019 - ROLI Ltd.
   |_____|_____|_____|_____|

   The code in this file is provided under the terms of the ISC license:

   Permission to use, copy, modify, and/or distribute this software for any purpose
   with or without fee is hereby granted, provided that the above copyright notice and
   this permission notice appear in all copies.

   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
   TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
   NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
   DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
   IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
   CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

namespace soul
{

//==============================================================================
/**
    A wrapper to simplify the job of rendering a Performer which only needs to deal
    with a synchronous set of audio, MIDI and parameter data (i.e. standard plugin stuff).
*/
struct AudioMIDIWrapper
{
    AudioMIDIWrapper (Performer& p)  : performer (p)
    {
    }

    ~AudioMIDIWrapper() = default;

    void reset()
    {
        totalFramesRendered = 0;
        preRenderOperations.clear();
        postRenderOperations.clear();
        numInputChannelsExpected = 0;
        numOutputChannelsExpected = 0;
        maxBlockSize = 0;
    }

    /** A lambda to create a function which will provide a non-null Value if called when the given parameter
        has been changed since the last call.
    */
    using GetNewParameterValueFn = std::function<std::function<const float*()>(const EndpointDetails& inputEndpoint)>;

    /**
    */
    using GetRampLengthForSparseStreamFn = std::function<uint32_t(const EndpointDetails&)>;

    /**
    */
    using HandleUnusedEventFn = std::function<bool(uint64_t eventTime, const std::string& endpointName, const choc::value::ValueView&)>;

    /**
    */
    void buildRenderingPipeline (uint32_t processorMaxBlockSize,
                                 GetNewParameterValueFn&& getNewParameterValueFn,
                                 GetRampLengthForSparseStreamFn&& getRampLengthForSparseStreamFn,
                                 HandleUnusedEventFn&& handleUnusedEventFn)
    {
        SOUL_ASSERT (processorMaxBlockSize > 0);
        reset();
        auto& perf = performer;
        maxBlockSize = std::min (512u, processorMaxBlockSize);

        for (auto& inputEndpoint : perf.getInputEndpoints())
        {
            if (isParameterInput (inputEndpoint))
            {
                if (getNewParameterValueFn != nullptr)
                {
                    if (auto getNewValueForParamIfChanged = getNewParameterValueFn (inputEndpoint))
                    {
                        auto endpointHandle = perf.getEndpointHandle (inputEndpoint.endpointID);
                        auto floatValue = choc::value::createFloat32 (0);

                        if (isEvent (inputEndpoint))
                        {
                            preRenderOperations.push_back ([&perf, endpointHandle, floatValue, getNewValueForParamIfChanged] (RenderContext&) mutable
                            {
                                if (auto newValue = getNewValueForParamIfChanged())
                                {
                                    floatValue.getViewReference().set (*newValue);
                                    perf.addInputEvent (endpointHandle, floatValue);
                                }
                            });
                        }
                        else if (isStream (inputEndpoint))
                        {
                            SOUL_ASSERT (getRampLengthForSparseStreamFn != nullptr);
                            auto rampFrames = getRampLengthForSparseStreamFn (inputEndpoint);

                            preRenderOperations.push_back ([&perf, endpointHandle, floatValue, getNewValueForParamIfChanged, rampFrames] (RenderContext&) mutable
                            {
                                if (auto newValue = getNewValueForParamIfChanged())
                                {
                                    floatValue.getViewReference().set (*newValue);
                                    perf.setSparseInputStreamTarget (endpointHandle, floatValue, rampFrames, 0.0f);
                                }
                            });
                        }
                        else if (isValue (inputEndpoint))
                        {
                            preRenderOperations.push_back ([&perf, endpointHandle, floatValue, getNewValueForParamIfChanged] (RenderContext&) mutable
                            {
                                if (auto newValue = getNewValueForParamIfChanged())
                                {
                                    floatValue.getViewReference().set (*newValue);
                                    perf.setInputValue (endpointHandle, floatValue);
                                }
                            });
                        }
                    }
                }
            }
            else if (isMIDIEventEndpoint (inputEndpoint))
            {
                auto endpointHandle = perf.getEndpointHandle (inputEndpoint.endpointID);
                choc::value::Value midiEvent (inputEndpoint.getSingleEventType());

                preRenderOperations.push_back ([&perf, endpointHandle, midiEvent] (RenderContext& rc) mutable
                {
                    for (uint32_t i = 0; i < rc.midiInCount; ++i)
                    {
                        midiEvent.getObjectMemberAt (0).value.set (rc.midiIn[i].getPackedMIDIData());
                        perf.addInputEvent (endpointHandle, midiEvent);
                    }
                });
            }
            else if (auto numSourceChans = inputEndpoint.getNumAudioChannels())
            {
                auto endpointHandle = perf.getEndpointHandle (inputEndpoint.endpointID);
                auto& frameType = inputEndpoint.getFrameType();
                auto startChannel = numInputChannelsExpected;
                auto numChans = frameType.getNumElements();

                if (frameType.isFloat() || (frameType.isVector() && frameType.getElementType().isFloat()))
                {
                    if (numChans == 1)
                    {
                        preRenderOperations.push_back ([&perf, endpointHandle, startChannel] (RenderContext& rc)
                        {
                            auto channel = rc.inputChannels.getChannel (startChannel);
                            perf.setNextInputStreamFrames (endpointHandle, choc::value::createArrayView (const_cast<float*> (channel.data.data),
                                                                                                         channel.getNumFrames()));
                        });
                    }
                    else
                    {
                        choc::buffer::InterleavedBuffer<float> interleaved (numChans, maxBlockSize);

                        preRenderOperations.push_back ([&perf, endpointHandle, startChannel, numChans, interleaved] (RenderContext& rc)
                        {
                            auto numFrames = rc.inputChannels.getNumFrames();

                            copy (interleaved.getStart (numFrames),
                                  rc.inputChannels.getChannelRange ({ startChannel, startChannel + numChans }));

                            perf.setNextInputStreamFrames (endpointHandle, choc::value::create2DArrayView (interleaved.getView().data.data,
                                                                                                           numFrames, interleaved.getNumChannels()));
                        });
                    }
                }
                else
                {
                    SOUL_ASSERT_FALSE;
                }

                numInputChannelsExpected += numSourceChans;
            }
        }

        for (auto& outputEndpoint : perf.getOutputEndpoints())
        {
            if (isMIDIEventEndpoint (outputEndpoint))
            {
                auto endpointHandle = perf.getEndpointHandle (outputEndpoint.endpointID);

                postRenderOperations.push_back ([&perf, endpointHandle] (RenderContext& rc)
                {
                    perf.iterateOutputEvents (endpointHandle, [&rc] (uint32_t frameOffset, const choc::value::ValueView& event) -> bool
                    {
                        if (rc.midiOutCount < rc.midiOutCapacity)
                            rc.midiOut[rc.midiOutCount++] = MIDIEvent::fromPackedMIDIData (rc.frameOffset + frameOffset,
                                                                                           event["midiBytes"].getInt32());

                        return true;
                    });
                });
            }
            else if (auto numChans = outputEndpoint.getNumAudioChannels())
            {
                auto endpointHandle = perf.getEndpointHandle (outputEndpoint.endpointID);
                auto& frameType = outputEndpoint.getFrameType();
                auto startChannel = numOutputChannelsExpected;
                numOutputChannelsExpected += numChans;

                if (frameType.isFloat() || (frameType.isVector() && frameType.getElementType().isFloat()))
                {
                    postRenderOperations.push_back ([&perf, endpointHandle, startChannel, numChans] (RenderContext& rc)
                    {
                        copyIntersectionAndClearOutside (rc.outputChannels.getChannelRange ({ startChannel, startChannel + numChans }),
                                                         getChannelSetFromArray (perf.getOutputStreamFrames (endpointHandle)));
                    });
                }
                else
                {
                    SOUL_ASSERT_FALSE;
                }
            }
            else if (handleUnusedEventFn != nullptr && isEvent (outputEndpoint))
            {
                auto endpointHandle = perf.getEndpointHandle (outputEndpoint.endpointID);
                auto endpointName = outputEndpoint.name;

                postRenderOperations.push_back ([&perf, endpointHandle, handleUnusedEventFn, endpointName] (RenderContext& rc)
                {
                    perf.iterateOutputEvents (endpointHandle, [&] (uint32_t frameOffset, const choc::value::ValueView& eventData) -> bool
                    {
                        return handleUnusedEventFn (rc.totalFramesRendered + frameOffset, endpointName, eventData);
                    });
                });
            }
        }
    }

    void render (choc::buffer::ChannelArrayView<const float> input,
                 choc::buffer::ChannelArrayView<float> output,
                 const MIDIEvent* midiIn,
                 MIDIEvent* midiOut,
                 uint32_t midiInCount,
                 uint32_t midiOutCapacity,
                 uint32_t& numMIDIOutMessages)
    {
        SOUL_ASSERT (input.getNumFrames() == output.getNumFrames() && maxBlockSize != 0);

        RenderContext context { totalFramesRendered, input, output, midiIn, midiOut, 0, midiInCount, 0, midiOutCapacity };

        context.iterateInBlocks (maxBlockSize, [&] (RenderContext& rc)
        {
            performer.prepare (rc.inputChannels.getNumFrames());

            for (auto& op : preRenderOperations)
                op (rc);

            performer.advance();

            for (auto& op : postRenderOperations)
                op (rc);
        });

        numMIDIOutMessages = context.midiOutCount;
        totalFramesRendered += input.getNumFrames();
    }

    uint32_t getExpectedNumInputChannels() const     { return numInputChannelsExpected; }
    uint32_t getExpectedNumOutputChannels() const    { return numOutputChannelsExpected; }

    struct RenderContext
    {
        uint64_t totalFramesRendered = 0;
        choc::buffer::ChannelArrayView<const float> inputChannels;
        choc::buffer::ChannelArrayView<float> outputChannels;
        const MIDIEvent* midiIn;
        MIDIEvent* midiOut;
        uint32_t frameOffset = 0, midiInCount = 0, midiOutCount = 0, midiOutCapacity = 0;

        template <typename RenderBlockFn>
        void iterateInBlocks (uint32_t maxFramesPerBlock, RenderBlockFn&& render)
        {
            auto framesRemaining = inputChannels.getNumFrames();
            auto context = *this;

            while (framesRemaining != 0)
            {
                auto framesToDo = std::min (maxFramesPerBlock, framesRemaining);
                context.midiIn = midiIn;
                context.midiInCount = 0;

                while (midiInCount != 0)
                {
                    auto time = midiIn->frameIndex;

                    if (time > frameOffset)
                    {
                        framesToDo = std::min (framesToDo, time - frameOffset);
                        break;
                    }

                    ++midiIn;
                    --midiInCount;
                    context.midiInCount++;
                }

                context.inputChannels  = inputChannels.getFrameRange ({ frameOffset, frameOffset + framesToDo });
                context.outputChannels = outputChannels.getFrameRange ({ frameOffset, frameOffset + framesToDo });

                render (context);

                frameOffset += framesToDo;
                framesRemaining -= framesToDo;
                context.totalFramesRendered += framesToDo;
                context.frameOffset += framesToDo;
            }

            midiOutCount = context.midiOutCount;
        }
    };

private:
    //==============================================================================
    Performer& performer;

    uint64_t totalFramesRendered = 0;
    std::vector<std::function<void(RenderContext&)>> preRenderOperations;
    std::vector<std::function<void(RenderContext&)>> postRenderOperations;
    uint32_t numInputChannelsExpected = 0, numOutputChannelsExpected = 0;
    uint32_t maxBlockSize = 0;
};

}
