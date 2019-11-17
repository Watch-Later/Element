/*
    This file is part of Element
    Copyright (C) 2019  Kushview, LLC.  All rights reserved.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "sol/sol.hpp"
#include "scripting/LuaBindings.h"
#include "engine/nodes/LuaNode.h"
#include "engine/MidiPipe.h"

static const String defaultScript = 
R"(
--- Lua Node template
--
-- This script came with Element and is in the public domain.
--
-- The code contained provides stereo audio in and out with one MIDI input
-- and one MIDI output.  It clears the audio buffer and logs midi messages
-- to the console.
--
-- The Lua filter node is highly experimental and the API is subject to change
-- without warning.  Please bear with us as we move toward a stable version. If
-- you are a developer and want to help out, see https://github.com/kushview/element

function node_io_ports()
    return {
        audio_ins   = 2,
        audio_outs  = 2,
        midi_ins    = 1,
        midi_outs   = 1
    }
end

-- Return parameters
function node_params()
    return {
        {
            name    = "Volume",
            label   = "dB",
            type    = "float",
            flow    = "input",
            min     = -90.0,
            max     = 24.0,
            default = 0.0
        }
    }
end

-- prepare for rendering
function node_prepare (rate, block)
    print (string.format ('prepare rate = %d block = %d', rate, block))
end

-- render audio and midi
function node_render (audio, midi)
    audio:clear()
    local mb = midi:get_read_buffer (0)
    for msg, _ in mb:iter() do
        print (msg)
    end
    mb:clear()
end

--- Release node resources
--  free any allocated resources in this callback
function node_release()
end

)";

namespace Element {

struct LuaNode::Context
{
    explicit Context () { }
    ~Context() { }

    String getName() const { return name; }

    bool ready() const { return loaded; }

    Result load (const String& script)
    {
        if (ready())
            return Result::fail ("Script already loaded");

        String errorMsg;
        try
        {
            state.open_libraries (sol::lib::base, sol::lib::string);
            Lua::registerEngine (state);
            auto res = state.script (script.toRawUTF8());
            if (res.valid())
            {
                renderf = state ["node_render"];
                renderstdf = renderf;
                loaded = true;
            }
        }
        catch (const std::exception& e)
        {
            errorMsg = e.what();
            loaded = false;
            renderstdf = nullptr;
        }

        return loaded ? Result::ok() : Result::fail ("Couldn't load Lua script");
    }

    static Result validate (const String& script)
    {
        if (script.isEmpty())
            return Result::fail ("script contains no code");

        auto ctx = std::make_unique<Context>();
        auto result = ctx->load (script);
        if (result.failed())
            return result;
        if (! ctx->ready())
            return Result::fail ("could not parse script");

        try
        {
            const int block = 1024;
            const double rate = 44100.0;

            using PT = kv::PortType;
            
            // call node_io_ports() and node_params()
            kv::PortList ports;
            ctx->createPorts (ports);

            // create a dummy audio buffer and midipipe
            auto nchans = jmax (ports.size (PT::Audio, true),
                                ports.size (PT::Audio, false));
            auto nmidi  = jmax (ports.size (PT::Midi, true),
                                ports.size (PT::Midi, false));
            AudioSampleBuffer audio (jmax (1, nchans), block);
            OwnedArray<MidiBuffer> midiBufs;
            Array<int> midiIdx;
            
            while (midiBufs.size() < nmidi)
            {
                midiIdx.add (midiBufs.size());
                midiBufs.add (new MidiBuffer ());
            }


            // calls node_prepare(), node_render(), and node_release()
            {
                auto midi = midiBufs.size() > 0 
                    ? std::make_unique<MidiPipe> (midiBufs, midiIdx)
                    : std::make_unique<MidiPipe>();
                ctx->prepare (rate, block);
                
                // user renderf directly so it can throw an exception
                if (ctx->renderf)
                    ctx->renderf (std::ref (audio), std::ref (*midi));

                ctx->release();
            }

            midiBufs.clearQuick (true);
            ctx.reset();
            result = Result::ok();
        }
        catch (const std::exception& e)
        {
            result = Result::fail (e.what());
        }

        return result;
    }

    void prepare (double rate, int block)
    {
        if (! ready())
            return;

        if (auto fn = state ["node_prepare"])
            fn (rate, block);
        
        state.collect_garbage();
    }

    void release()
    {
        if (! ready())
            return;

        if (auto fn = state ["node_release"])
            fn();
        
        state.collect_garbage();
    }

    void render (AudioSampleBuffer& audio, MidiPipe& midi) noexcept
    {
        if (loaded)
            renderstdf (audio, midi);
    }

    void createPorts (kv::PortList& ports)
    {
        if (! ready())
            return;
        addIOPorts (ports);
        addParameters (ports);
    }

private:
    sol::state state;
    sol::function renderf;
    std::function<void(AudioSampleBuffer&, MidiPipe&)> renderstdf;
    String name;
    bool loaded = false;

    void addParameters (kv::PortList& ports)
    {
        if (auto f = state ["node_params"])
        {
            int index = ports.size();
            int inChan = 0, outChan = 0;

            try {
                sol::table params = f();
                for (int i = 0; i < params.size(); ++i)
                {
                    String name  = params[i + 1]["name"].get_or (std::string ("Param"));
                    String sym   = name.trim().toLowerCase().replace(" ", "_");
                    String type  = params[i + 1]["type"].get_or (std::string ("float"));
                    String flow  = params[i + 1]["flow"].get_or (std::string ("input"));
                    jassert (flow == "input" || flow == "output");
                    
                    bool isInput = flow == "input";
                    float min    = params[i + 1]["min"].get_or (0.0);
                    float max    = params[i + 1]["max"].get_or (1.0);
                    float dfault = params[i + 1]["default"].get_or (1.0);
                    ignoreUnused (min, max, dfault);
                    const int channel = isInput ? inChan++ : outChan++;
                   #if 0
                    DBG("index = " << index);
                    DBG("channel = " << channel);
                    DBG("is input = " << (int) isInput);
                    DBG("name = " << name);
                    DBG("symbol = " << sym);
                   #endif
                    ports.add (kv::PortType::Control, index++, channel, sym, name, isInput);
                }
            }
            catch (const std::exception&)
            {

            }
        }
    }

    void addIOPorts (kv::PortList& ports)
    {
        auto& lua = state;

        if (auto f = lua ["node_io_ports"])
        {
            sol::table t = f();
            int audioIns = 0, audioOuts = 0,
                midiIns = 0, midiOuts = 0;

            try {
                if (t.size() == 0)
                {
                    audioIns  = t["audio_ins"].get_or (0);
                    audioOuts = t["audio_outs"].get_or (0);
                    midiIns   = t["midi_ins"].get_or (0);
                    midiOuts  = t["midi_outs"].get_or (0);
                }
                else
                {
                    audioIns  = t[1]["audio_ins"].get_or (0);
                    audioOuts = t[1]["audio_outs"].get_or (0);
                    midiIns   = t[1]["midi_ins"].get_or (0);
                    midiOuts  = t[1]["midi_outs"].get_or (0);
                }
            }
            catch (const std::exception&) {}

            int index = 0, channel = 0;
            for (int i = 0; i < audioIns; ++i)
            {
                String slug = "in_"; slug << (i + 1);
                String name = "In "; name << (i + 1);
                ports.add (PortType::Audio, index++, channel++,
                           slug, name, true);
            }

            channel = 0;
            for (int i = 0; i < audioOuts; ++i)
            {
                String slug = "out_"; slug << (i + 1);
                String name = "Out "; name << (i + 1);
                ports.add (PortType::Audio, index++, channel++,
                           slug, name, false);
            }

            channel = 0;
            for (int i = 0; i < midiIns; ++i)
            {
                String slug = "midi_in_"; slug << (i + 1);
                String name = "MIDI In "; name << (i + 1);
                ports.add (PortType::Midi, index++, channel++,
                           slug, name, true);
            }

            channel = 0;
            for (int i = 0; i < midiOuts; ++i)
            {
                String slug = "midi_out_"; slug << (i + 1);
                String name = "MIDI Out "; name << (i + 1);
                ports.add (PortType::Midi, index++, channel++,
                           slug, name, false);
            }
        }
    }
};

LuaNode::LuaNode() noexcept
    : GraphNode (0)
{
    context = std::make_unique<Context>();
    jassert (metadata.hasType (Tags::node));
    metadata.setProperty (Tags::format, EL_INTERNAL_FORMAT_NAME, nullptr);
    metadata.setProperty (Tags::identifier, EL_INTERNAL_ID_LUA, nullptr);
    loadScript (defaultScript);
}

LuaNode::~LuaNode()
{
    context.reset();
}

void LuaNode::createPorts()
{
    if (context == nullptr)
        return;

    ports.clearQuick();
    context->createPorts (ports);
}

Result LuaNode::loadScript (const String& newScript)
{
   #if 1
    auto result = Context::validate (newScript);
    if (result.failed())
        return result;
   #else
    auto result = Result::fail ("Unknown parsing error");
   #endif
    
    auto newContext = std::make_unique<Context>();
    result = newContext->load (newScript);

    if (result.wasOk())
    {
        script = draftScript = newScript;
        if (prepared)
            newContext->prepare (sampleRate, blockSize);
        ScopedLock sl (lock);
        context.swap (newContext);
    }

    if (newContext != nullptr)
    {
        newContext->release();
        newContext.reset();
    }

    return result;
}

void LuaNode::fillInPluginDescription (PluginDescription& desc)
{
    desc.name               = "Lua";
    desc.fileOrIdentifier   = EL_INTERNAL_ID_LUA;
    desc.uid                = EL_INTERNAL_UID_LUA;
    desc.descriptiveName    = "A user scriptable Element node";
    desc.numInputChannels   = 0;
    desc.numOutputChannels  = 0;
    desc.hasSharedContainer = false;
    desc.isInstrument       = false;
    desc.manufacturerName   = "Element";
    desc.pluginFormatName   = EL_INTERNAL_FORMAT_NAME;
    desc.version            = "1.0.0";
}

void LuaNode::prepareToRender (double rate, int block)
{
    if (prepared)
        return;
    sampleRate = rate;
    blockSize = block;
    context->prepare (sampleRate, blockSize);
    prepared = true;
}

void LuaNode::releaseResources()
{
    if (! prepared)
        return;
    prepared = false;
    context->release();
}

void LuaNode::render (AudioSampleBuffer& audio, MidiPipe& midi)
{
    ScopedLock sl (lock);
    context->render (audio, midi);
}

void LuaNode::setState (const void* data, int size)
{
    const auto state = ValueTree::readFromData (data, size);
    if (state.isValid())
    {
        loadScript (state["script"].toString());
        sendChangeMessage();
    }
}

void LuaNode::getState (MemoryBlock& block)
{
    ValueTree state ("lua");
    state.setProperty ("script", script, nullptr)
         .setProperty ("draft", draftScript, nullptr);
    MemoryOutputStream mo (block, false);
    state.writeToStream (mo);
}

}
