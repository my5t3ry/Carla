/*
 * Carla Plugin discovery
 * Copyright (C) 2011-2014 Filipe Coelho <falktx@falktx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the doc/GPL.txt file.
 */

// if using juce, don't build UI stuff
#define JUCE_PLUGIN_HOST_NO_UI

#include "CarlaBackendUtils.hpp"
#include "CarlaLibUtils.hpp"
#include "CarlaMathUtils.hpp"
#include "CarlaMIDI.h"

#ifdef HAVE_JUCE
# include "juce_audio_processors.h"
#else
# undef WANT_CSOUND
#endif

#ifdef WANT_LADSPA
# include "CarlaLadspaUtils.hpp"
#endif
#ifdef WANT_DSSI
# include "CarlaDssiUtils.hpp"
#endif
#ifdef WANT_LV2
# include "CarlaLv2Utils.hpp"
#endif
#ifdef WANT_VST
# include "CarlaVstUtils.hpp"
#endif
#ifdef WANT_CSOUND
# include <csound/csound.hpp>
#endif
#ifdef WANT_FLUIDSYNTH
# include <fluidsynth.h>
#endif
#ifdef WANT_LINUXSAMPLER
# include "linuxsampler/EngineFactory.h"
#endif

#include <iostream>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QUrl>

#define DISCOVERY_OUT(x, y) std::cout << "\ncarla-discovery::" << x << "::" << y << std::endl;

CARLA_BACKEND_USE_NAMESPACE

// --------------------------------------------------------------------------
// Dummy values to test plugins with

static const uint32_t kBufferSize  = 512;
static const double   kSampleRate  = 44100.0;
static const float    kSampleRatef = 44100.0f;

// --------------------------------------------------------------------------
// Don't print ELF/EXE related errors since discovery can find multi-architecture binaries

static void print_lib_error(const char* const filename)
{
    const char* const error(lib_error(filename));

    if (error != nullptr && std::strstr(error, "wrong ELF class") == nullptr && std::strstr(error, "Bad EXE format") == nullptr)
        DISCOVERY_OUT("error", error);
}

#ifdef WANT_VST
// --------------------------------------------------------------------------
// VST stuff

// Check if plugin is currently processing
static bool gVstIsProcessing = false;

// Check if plugin needs idle
static bool gVstNeedsIdle = false;

// Check if plugin wants midi
static bool gVstWantsMidi = false;

// Check if plugin wants time
static bool gVstWantsTime = false;

// Current uniqueId for VST shell plugins
static intptr_t gVstCurrentUniqueId = 0;

// Supported Carla features
static intptr_t vstHostCanDo(const char* const feature)
{
    carla_debug("vstHostCanDo(\"%s\")", feature);

    if (std::strcmp(feature, "supplyIdle") == 0)
        return 1;
    if (std::strcmp(feature, "sendVstEvents") == 0)
        return 1;
    if (std::strcmp(feature, "sendVstMidiEvent") == 0)
        return 1;
    if (std::strcmp(feature, "sendVstMidiEventFlagIsRealtime") == 0)
        return 1;
    if (std::strcmp(feature, "sendVstTimeInfo") == 0)
    {
        gVstWantsTime = true;
        return 1;
    }
    if (std::strcmp(feature, "receiveVstEvents") == 0)
        return 1;
    if (std::strcmp(feature, "receiveVstMidiEvent") == 0)
        return 1;
    if (std::strcmp(feature, "receiveVstTimeInfo") == 0)
        return -1;
    if (std::strcmp(feature, "reportConnectionChanges") == 0)
        return -1;
    if (std::strcmp(feature, "acceptIOChanges") == 0)
        return 1;
    if (std::strcmp(feature, "sizeWindow") == 0)
        return 1;
    if (std::strcmp(feature, "offline") == 0)
        return -1;
    if (std::strcmp(feature, "openFileSelector") == 0)
        return -1;
    if (std::strcmp(feature, "closeFileSelector") == 0)
        return -1;
    if (std::strcmp(feature, "startStopProcess") == 0)
        return 1;
    if (std::strcmp(feature, "supportShell") == 0)
        return 1;
    if (std::strcmp(feature, "shellCategory") == 0)
        return 1;

    // non-official features found in some plugins:
    // "asyncProcessing"
    // "editFile"

    // unimplemented
    carla_stderr("vstHostCanDo(\"%s\") - unknown feature", feature);
    return 0;
}

// Host-side callback
static intptr_t VSTCALLBACK vstHostCallback(AEffect* const effect, const int32_t opcode, const int32_t index, const intptr_t value, void* const ptr, const float opt)
{
    carla_debug("vstHostCallback(%p, %i:%s, %i, " P_INTPTR ", %p, %f)", effect, opcode, vstMasterOpcode2str(opcode), index, value, ptr, opt);

    static VstTimeInfo_R timeInfo;
    intptr_t ret = 0;

    switch (opcode)
    {
    case audioMasterAutomate:
        ret = 1;
        break;

    case audioMasterVersion:
        ret = kVstVersion;
        break;

    case audioMasterCurrentId:
        if (gVstCurrentUniqueId == 0) DISCOVERY_OUT("warning", "plugin asked for uniqueId, but it's currently 0");

        ret = gVstCurrentUniqueId;
        break;

    case DECLARE_VST_DEPRECATED(audioMasterWantMidi):
        if (gVstWantsMidi) DISCOVERY_OUT("warning", "plugin requested MIDI more than once");

        gVstWantsMidi = true;
        ret = 1;
        break;

    case audioMasterGetTime:
        if (! gVstIsProcessing) DISCOVERY_OUT("warning", "plugin requested timeInfo out of process");
        if (! gVstWantsTime)    DISCOVERY_OUT("warning", "plugin requested timeInfo but didn't ask if host could do \"sendVstTimeInfo\"");

        carla_zeroStruct<VstTimeInfo_R>(timeInfo);
        timeInfo.sampleRate = kSampleRate;

        // Tempo
        timeInfo.tempo  = 120.0;
        timeInfo.flags |= kVstTempoValid;

        // Time Signature
        timeInfo.timeSigNumerator   = 4;
        timeInfo.timeSigDenominator = 4;
        timeInfo.flags |= kVstTimeSigValid;

        ret = (intptr_t)&timeInfo;
        break;

    case DECLARE_VST_DEPRECATED(audioMasterTempoAt):
        ret = 120 * 10000;
        break;

    case DECLARE_VST_DEPRECATED(audioMasterGetNumAutomatableParameters):
        ret = carla_min<intptr_t>(effect->numParams, MAX_DEFAULT_PARAMETERS, 0);
        break;

    case DECLARE_VST_DEPRECATED(audioMasterGetParameterQuantization):
        ret = 1; // full single float precision
        break;

    case DECLARE_VST_DEPRECATED(audioMasterNeedIdle):
        if (gVstNeedsIdle) DISCOVERY_OUT("warning", "plugin requested idle more than once");

        gVstNeedsIdle = true;
        ret = 1;
        break;

    case audioMasterGetSampleRate:
        ret = kSampleRate;
        break;

    case audioMasterGetBlockSize:
        ret = kBufferSize;
        break;

    case DECLARE_VST_DEPRECATED(audioMasterWillReplaceOrAccumulate):
        ret = 1; // replace
        break;

    case audioMasterGetCurrentProcessLevel:
        ret = gVstIsProcessing ? kVstProcessLevelRealtime : kVstProcessLevelUser;
        break;

    case audioMasterGetAutomationState:
        ret = kVstAutomationOff;
        break;

    case audioMasterGetVendorString:
        CARLA_SAFE_ASSERT_BREAK(ptr != nullptr);
        std::strcpy((char*)ptr, "falkTX");
        ret = 1;
        break;

    case audioMasterGetProductString:
        CARLA_SAFE_ASSERT_BREAK(ptr != nullptr);
        std::strcpy((char*)ptr, "Carla-Discovery");
        ret = 1;
        break;

    case audioMasterGetVendorVersion:
        ret = CARLA_VERSION_HEX;
        break;

    case audioMasterCanDo:
        CARLA_SAFE_ASSERT_BREAK(ptr != nullptr);
        ret = vstHostCanDo((const char*)ptr);
        break;

    case audioMasterGetLanguage:
        ret = kVstLangEnglish;
        break;

    default:
        carla_stdout("vstHostCallback(%p, %i:%s, %i, " P_INTPTR ", %p, %f)", effect, opcode, vstMasterOpcode2str(opcode), index, value, ptr, opt);
        break;
    }

    return ret;
}
#endif

#ifdef WANT_CSOUND
// --------------------------------------------------------------------------
// Csound stuff

static int csound_midiInOpen(CSOUND*,  void**, const char*)        { return 0; }
static int csound_midiRead(CSOUND*,    void*, unsigned char*, int) { return 0; }
static int csound_midiInClose(CSOUND*, void*)                      { return 0; }

static int csound_midiOutOpen(CSOUND*,  void**, const char*)               { return 0; }
static int csound_midiWrite(CSOUND*,    void*,  const unsigned char*, int) { return 0; }
static int csound_midiOutClose(CSOUND*, void*)                             { return 0; }

# ifndef DEBUG
static void csound_silence(CSOUND*, int, const char*, va_list) {}
# endif
#endif

#ifdef WANT_LINUXSAMPLER
// --------------------------------------------------------------------------
// LinuxSampler stuff

class LinuxSamplerScopedEngine
{
public:
    LinuxSamplerScopedEngine(const char* const filename, const char* const stype)
        : fEngine(nullptr)
    {
        using namespace LinuxSampler;

        try {
            fEngine = EngineFactory::Create(stype);
        }
        catch (const Exception& e)
        {
            DISCOVERY_OUT("error", e.what());
            return;
        }

        if (fEngine == nullptr)
            return;

        InstrumentManager* const insMan(fEngine->GetInstrumentManager());

        if (insMan == nullptr)
        {
            DISCOVERY_OUT("error", "Failed to get LinuxSampler instrument manager");
            return;
        }

        std::vector<InstrumentManager::instrument_id_t> ids;

        try {
            ids = insMan->GetInstrumentFileContent(filename);
        }
        catch (const InstrumentManagerException& e)
        {
            DISCOVERY_OUT("error", e.what());
            return;
        }

        if (ids.size() == 0)
        {
            DISCOVERY_OUT("error", "Failed to find any instruments");
            return;
        }

        InstrumentManager::instrument_info_t info;

        try {
            info = insMan->GetInstrumentInfo(ids[0]);
        }
        catch (const InstrumentManagerException& e)
        {
            DISCOVERY_OUT("error", e.what());
            return;
        }

        outputInfo(&info, ids.size());
    }

    ~LinuxSamplerScopedEngine()
    {
        if (fEngine != nullptr)
        {
            LinuxSampler::EngineFactory::Destroy(fEngine);
            fEngine = nullptr;
        }
    }

    static void outputInfo(const LinuxSampler::InstrumentManager::instrument_info_t* const info, const size_t programs, const char* const basename = nullptr)
    {
        CarlaString name;
        const char* label;

        if (info != nullptr)
        {
            name  = info->InstrumentName.c_str();
            label = info->Product.c_str();
        }
        else
        {
            name  = basename;
            label = basename;
        }

        // 2 channels
        DISCOVERY_OUT("init", "-----------");
        DISCOVERY_OUT("name", (const char*)name);
        DISCOVERY_OUT("label", label);

        if (info != nullptr)
        {
            DISCOVERY_OUT("maker", info->Artists);
            DISCOVERY_OUT("copyright", info->Artists);
        }

        DISCOVERY_OUT("hints", PLUGIN_IS_SYNTH);
        DISCOVERY_OUT("audio.outs", 2);
        DISCOVERY_OUT("midi.ins", 1);
        DISCOVERY_OUT("programs", programs);
        DISCOVERY_OUT("build", BINARY_NATIVE);
        DISCOVERY_OUT("end", "------------");

        if (name.isEmpty() || programs <= 1)
            return;
        name += " (16 outputs)";

        // 16 channels
        DISCOVERY_OUT("init", "-----------");
        DISCOVERY_OUT("name", (const char*)name);
        DISCOVERY_OUT("label", label);

        if (info != nullptr)
        {
            DISCOVERY_OUT("maker", info->Artists);
            DISCOVERY_OUT("copyright", info->Artists);
        }

        DISCOVERY_OUT("hints", PLUGIN_IS_SYNTH);
        DISCOVERY_OUT("audio.outs", 2);
        DISCOVERY_OUT("midi.ins", 1);
        DISCOVERY_OUT("programs", programs);
        DISCOVERY_OUT("build", BINARY_NATIVE);
        DISCOVERY_OUT("end", "------------");
    }

private:
    LinuxSampler::Engine* fEngine;

    CARLA_PREVENT_HEAP_ALLOCATION
    CARLA_DECLARE_NON_COPY_CLASS(LinuxSamplerScopedEngine)
};
#endif

// ------------------------------ Plugin Checks -----------------------------

static void do_ladspa_check(void*& libHandle, const char* const filename, const bool init)
{
#ifdef WANT_LADSPA
    LADSPA_Descriptor_Function descFn = (LADSPA_Descriptor_Function)lib_symbol(libHandle, "ladspa_descriptor");

    if (descFn == nullptr)
    {
        DISCOVERY_OUT("error", "Not a LADSPA plugin");
        return;
    }

    const LADSPA_Descriptor* descriptor;

    {
        descriptor = descFn(0);

        if (descriptor == nullptr)
        {
            DISCOVERY_OUT("error", "Binary doesn't contain any plugins");
            return;
        }

        if (init && descriptor->instantiate != nullptr && descriptor->cleanup != nullptr)
        {
            LADSPA_Handle handle = descriptor->instantiate(descriptor, kSampleRate);

            if (handle == nullptr)
            {
                DISCOVERY_OUT("error", "Failed to init first LADSPA plugin");
                return;
            }

            descriptor->cleanup(handle);

            lib_close(libHandle);
            libHandle = lib_open(filename);

            if (libHandle == nullptr)
            {
                print_lib_error(filename);
                return;
            }

            descFn = (LADSPA_Descriptor_Function)lib_symbol(libHandle, "ladspa_descriptor");

            if (descFn == nullptr)
            {
                DISCOVERY_OUT("error", "Not a LADSPA plugin (#2)");
                return;
            }
        }
    }

    unsigned long i = 0;

    while ((descriptor = descFn(i++)) != nullptr)
    {
        if (descriptor->instantiate == nullptr)
        {
            DISCOVERY_OUT("error", "Plugin '" << descriptor->Name << "' has no instantiate()");
            continue;
        }
        if (descriptor->cleanup == nullptr)
        {
            DISCOVERY_OUT("error", "Plugin '" << descriptor->Name << "' has no cleanup()");
            continue;
        }
        if (descriptor->run == nullptr)
        {
            DISCOVERY_OUT("error", "Plugin '" << descriptor->Name << "' has no run()");
            continue;
        }
        if (! LADSPA_IS_HARD_RT_CAPABLE(descriptor->Properties))
        {
            DISCOVERY_OUT("warning", "Plugin '" << descriptor->Name << "' is not hard real-time capable");
        }

        uint hints = 0x0;
        int audioIns = 0;
        int audioOuts = 0;
        int audioTotal = 0;
        int parametersIns = 0;
        int parametersOuts = 0;
        int parametersTotal = 0;

        if (LADSPA_IS_HARD_RT_CAPABLE(descriptor->Properties))
            hints |= PLUGIN_IS_RTSAFE;

        for (unsigned long j=0; j < descriptor->PortCount; ++j)
        {
            CARLA_ASSERT(descriptor->PortNames[j] != nullptr);
            const LADSPA_PortDescriptor portDescriptor = descriptor->PortDescriptors[j];

            if (LADSPA_IS_PORT_AUDIO(portDescriptor))
            {
                if (LADSPA_IS_PORT_INPUT(portDescriptor))
                    audioIns += 1;
                else if (LADSPA_IS_PORT_OUTPUT(portDescriptor))
                    audioOuts += 1;

                audioTotal += 1;
            }
            else if (LADSPA_IS_PORT_CONTROL(portDescriptor))
            {
                if (LADSPA_IS_PORT_INPUT(portDescriptor))
                    parametersIns += 1;
                else if (LADSPA_IS_PORT_OUTPUT(portDescriptor) && std::strcmp(descriptor->PortNames[j], "latency") != 0 && std::strcmp(descriptor->PortNames[j], "_latency") != 0)
                    parametersOuts += 1;

                parametersTotal += 1;
            }
        }

        if (init)
        {
            // -----------------------------------------------------------------------
            // start crash-free plugin test

            LADSPA_Handle handle = descriptor->instantiate(descriptor, kSampleRate);

            if (handle == nullptr)
            {
                DISCOVERY_OUT("error", "Failed to init LADSPA plugin");
                continue;
            }

            // Test quick init and cleanup
            descriptor->cleanup(handle);

            handle = descriptor->instantiate(descriptor, kSampleRate);

            if (handle == nullptr)
            {
                DISCOVERY_OUT("error", "Failed to init LADSPA plugin (#2)");
                continue;
            }

            LADSPA_Data bufferAudio[kBufferSize][audioTotal];
            LADSPA_Data bufferParams[parametersTotal];
            LADSPA_Data min, max, def;

            for (unsigned long j=0, iA=0, iC=0; j < descriptor->PortCount; ++j)
            {
                const LADSPA_PortDescriptor portDescriptor = descriptor->PortDescriptors[j];
                const LADSPA_PortRangeHint  portRangeHints = descriptor->PortRangeHints[j];
                const char* const portName = descriptor->PortNames[j];

                if (LADSPA_IS_PORT_AUDIO(portDescriptor))
                {
                    carla_zeroFloat(bufferAudio[iA], kBufferSize);
                    descriptor->connect_port(handle, j, bufferAudio[iA++]);
                }
                else if (LADSPA_IS_PORT_CONTROL(portDescriptor))
                {
                    // min value
                    if (LADSPA_IS_HINT_BOUNDED_BELOW(portRangeHints.HintDescriptor))
                        min = portRangeHints.LowerBound;
                    else
                        min = 0.0f;

                    // max value
                    if (LADSPA_IS_HINT_BOUNDED_ABOVE(portRangeHints.HintDescriptor))
                        max = portRangeHints.UpperBound;
                    else
                        max = 1.0f;

                    if (min > max)
                    {
                        DISCOVERY_OUT("warning", "Parameter '" << portName << "' is broken: min > max");
                        max = min + 0.1f;
                    }
                    else if (max - min == 0.0f)
                    {
                        DISCOVERY_OUT("warning", "Parameter '" << portName << "' is broken: max - min == 0");
                        max = min + 0.1f;
                    }

                    // default value
                    def = get_default_ladspa_port_value(portRangeHints.HintDescriptor, min, max);

                    if (LADSPA_IS_HINT_SAMPLE_RATE(portRangeHints.HintDescriptor))
                    {
                        min *= kSampleRatef;
                        max *= kSampleRatef;
                        def *= kSampleRatef;
                    }

                    if (LADSPA_IS_PORT_OUTPUT(portDescriptor) && (std::strcmp(portName, "latency") == 0 || std::strcmp(portName, "_latency") == 0))
                    {
                        // latency parameter
                        def = 0.0f;
                    }
                    else
                    {
                        if (def < min)
                            def = min;
                        else if (def > max)
                            def = max;
                    }

                    bufferParams[iC] = def;
                    descriptor->connect_port(handle, j, &bufferParams[iC++]);
                }
            }

            if (descriptor->activate != nullptr)
                descriptor->activate(handle);

            descriptor->run(handle, kBufferSize);

            if (descriptor->deactivate != nullptr)
                descriptor->deactivate(handle);

            descriptor->cleanup(handle);

            // end crash-free plugin test
            // -----------------------------------------------------------------------
        }

        DISCOVERY_OUT("init", "-----------");
        DISCOVERY_OUT("name", descriptor->Name);
        DISCOVERY_OUT("label", descriptor->Label);
        DISCOVERY_OUT("maker", descriptor->Maker);
        DISCOVERY_OUT("copyright", descriptor->Copyright);
        DISCOVERY_OUT("uniqueId", descriptor->UniqueID);
        DISCOVERY_OUT("hints", hints);
        DISCOVERY_OUT("audio.ins", audioIns);
        DISCOVERY_OUT("audio.outs", audioOuts);
        DISCOVERY_OUT("parameters.ins", parametersIns);
        DISCOVERY_OUT("parameters.outs", parametersOuts);
        DISCOVERY_OUT("build", BINARY_NATIVE);
        DISCOVERY_OUT("end", "------------");
    }
#else
    DISCOVERY_OUT("error", "LADSPA support not available");
    return;

    // unused
    (void)libHandle;
    (void)filename;
    (void)init;
#endif
}

static void do_dssi_check(void*& libHandle, const char* const filename, const bool init)
{
#ifdef WANT_DSSI
    DSSI_Descriptor_Function descFn = (DSSI_Descriptor_Function)lib_symbol(libHandle, "dssi_descriptor");

    if (descFn == nullptr)
    {
        DISCOVERY_OUT("error", "Not a DSSI plugin");
        return;
    }

    const DSSI_Descriptor* descriptor;

    {
        descriptor = descFn(0);

        if (descriptor == nullptr)
        {
            DISCOVERY_OUT("error", "Binary doesn't contain any plugins");
            return;
        }

        const LADSPA_Descriptor* const ldescriptor(descriptor->LADSPA_Plugin);

        if (ldescriptor == nullptr)
        {
            DISCOVERY_OUT("error", "DSSI plugin doesn't provide the LADSPA interface");
            return;
        }

        if (init && ldescriptor->instantiate != nullptr && ldescriptor->cleanup != nullptr)
        {
            LADSPA_Handle handle = ldescriptor->instantiate(ldescriptor, kSampleRate);

            if (handle == nullptr)
            {
                DISCOVERY_OUT("error", "Failed to init first LADSPA plugin");
                return;
            }

            ldescriptor->cleanup(handle);

            lib_close(libHandle);
            libHandle = lib_open(filename);

            if (libHandle == nullptr)
            {
                print_lib_error(filename);
                return;
            }

            descFn = (DSSI_Descriptor_Function)lib_symbol(libHandle, "dssi_descriptor");

            if (descFn == nullptr)
            {
                DISCOVERY_OUT("error", "Not a DSSI plugin (#2)");
                return;
            }
        }
    }

    unsigned long i = 0;

    while ((descriptor = descFn(i++)) != nullptr)
    {
        const LADSPA_Descriptor* const ldescriptor(descriptor->LADSPA_Plugin);

        if (ldescriptor == nullptr)
        {
            DISCOVERY_OUT("error", "Plugin '" << ldescriptor->Name << "' has no LADSPA interface");
            continue;
        }
        if (descriptor->DSSI_API_Version != DSSI_VERSION_MAJOR)
        {
            DISCOVERY_OUT("error", "Plugin '" << ldescriptor->Name << "' uses an unsupported DSSI spec version " << descriptor->DSSI_API_Version);
            continue;
        }
        if (ldescriptor->instantiate == nullptr)
        {
            DISCOVERY_OUT("error", "Plugin '" << ldescriptor->Name << "' has no instantiate()");
            continue;
        }
        if (ldescriptor->cleanup == nullptr)
        {
            DISCOVERY_OUT("error", "Plugin '" << ldescriptor->Name << "' has no cleanup()");
            continue;
        }
        if (ldescriptor->run == nullptr && descriptor->run_synth == nullptr && descriptor->run_multiple_synths == nullptr)
        {
            DISCOVERY_OUT("error", "Plugin '" << ldescriptor->Name << "' has no run(), run_synth() or run_multiple_synths()");
            continue;
        }
        if (! LADSPA_IS_HARD_RT_CAPABLE(ldescriptor->Properties))
        {
            DISCOVERY_OUT("warning", "Plugin '" << ldescriptor->Name << "' is not hard real-time capable");
        }

        uint hints = 0x0;
        int audioIns = 0;
        int audioOuts = 0;
        int audioTotal = 0;
        int midiIns = 0;
        int parametersIns = 0;
        int parametersOuts = 0;
        int parametersTotal = 0;
        ulong programs = 0;

        if (LADSPA_IS_HARD_RT_CAPABLE(ldescriptor->Properties))
            hints |= PLUGIN_IS_RTSAFE;

        for (unsigned long j=0; j < ldescriptor->PortCount; ++j)
        {
            CARLA_ASSERT(ldescriptor->PortNames[j] != nullptr);
            const LADSPA_PortDescriptor portDescriptor = ldescriptor->PortDescriptors[j];

            if (LADSPA_IS_PORT_AUDIO(portDescriptor))
            {
                if (LADSPA_IS_PORT_INPUT(portDescriptor))
                    audioIns += 1;
                else if (LADSPA_IS_PORT_OUTPUT(portDescriptor))
                    audioOuts += 1;

                audioTotal += 1;
            }
            else if (LADSPA_IS_PORT_CONTROL(portDescriptor))
            {
                if (LADSPA_IS_PORT_INPUT(portDescriptor))
                    parametersIns += 1;
                else if (LADSPA_IS_PORT_OUTPUT(portDescriptor) && std::strcmp(ldescriptor->PortNames[j], "latency") != 0 && std::strcmp(ldescriptor->PortNames[j], "_latency") != 0)
                    parametersOuts += 1;

                parametersTotal += 1;
            }
        }

        if (descriptor->run_synth != nullptr || descriptor->run_multiple_synths != nullptr)
            midiIns = 1;

        if (midiIns > 0 && audioIns == 0 && audioOuts > 0)
            hints |= PLUGIN_IS_SYNTH;

        if (const char* const ui = find_dssi_ui(filename, ldescriptor->Label))
        {
            hints |= PLUGIN_HAS_CUSTOM_UI;
            delete[] ui;
        }

        if (init)
        {
            // -----------------------------------------------------------------------
            // start crash-free plugin test

            LADSPA_Handle handle = ldescriptor->instantiate(ldescriptor, kSampleRate);

            if (handle == nullptr)
            {
                DISCOVERY_OUT("error", "Failed to init DSSI plugin");
                continue;
            }

            // Test quick init and cleanup
            ldescriptor->cleanup(handle);

            handle = ldescriptor->instantiate(ldescriptor, kSampleRate);

            if (handle == nullptr)
            {
                DISCOVERY_OUT("error", "Failed to init DSSI plugin (#2)");
                continue;
            }

            if (descriptor->get_program != nullptr && descriptor->select_program != nullptr)
            {
                for (; descriptor->get_program(handle, programs) != nullptr;)
                    ++programs;
            }

            LADSPA_Data bufferAudio[kBufferSize][audioTotal];
            LADSPA_Data bufferParams[parametersTotal];
            LADSPA_Data min, max, def;

            for (unsigned long j=0, iA=0, iC=0; j < ldescriptor->PortCount; ++j)
            {
                const LADSPA_PortDescriptor portDescriptor = ldescriptor->PortDescriptors[j];
                const LADSPA_PortRangeHint  portRangeHints = ldescriptor->PortRangeHints[j];
                const char* const portName = ldescriptor->PortNames[j];

                if (LADSPA_IS_PORT_AUDIO(portDescriptor))
                {
                    carla_zeroFloat(bufferAudio[iA], kBufferSize);
                    ldescriptor->connect_port(handle, j, bufferAudio[iA++]);
                }
                else if (LADSPA_IS_PORT_CONTROL(portDescriptor))
                {
                    // min value
                    if (LADSPA_IS_HINT_BOUNDED_BELOW(portRangeHints.HintDescriptor))
                        min = portRangeHints.LowerBound;
                    else
                        min = 0.0f;

                    // max value
                    if (LADSPA_IS_HINT_BOUNDED_ABOVE(portRangeHints.HintDescriptor))
                        max = portRangeHints.UpperBound;
                    else
                        max = 1.0f;

                    if (min > max)
                    {
                        DISCOVERY_OUT("warning", "Parameter '" << portName << "' is broken: min > max");
                        max = min + 0.1f;
                    }
                    else if (max - min == 0.0f)
                    {
                        DISCOVERY_OUT("warning", "Parameter '" << portName << "' is broken: max - min == 0");
                        max = min + 0.1f;
                    }

                    // default value
                    def = get_default_ladspa_port_value(portRangeHints.HintDescriptor, min, max);

                    if (LADSPA_IS_HINT_SAMPLE_RATE(portRangeHints.HintDescriptor))
                    {
                        min *= kSampleRatef;
                        max *= kSampleRatef;
                        def *= kSampleRatef;
                    }

                    if (LADSPA_IS_PORT_OUTPUT(portDescriptor) && (std::strcmp(portName, "latency") == 0 || std::strcmp(portName, "_latency") == 0))
                    {
                        // latency parameter
                        def = 0.0f;
                    }
                    else
                    {
                        if (def < min)
                            def = min;
                        else if (def > max)
                            def = max;
                    }

                    bufferParams[iC] = def;
                    ldescriptor->connect_port(handle, j, &bufferParams[iC++]);
                }
            }

            // select first midi-program if available
            if (programs > 0)
            {
                if (const DSSI_Program_Descriptor* const pDesc = descriptor->get_program(handle, 0))
                    descriptor->select_program(handle, pDesc->Bank, pDesc->Program);
            }

            if (ldescriptor->activate != nullptr)
                ldescriptor->activate(handle);

            if (descriptor->run_synth != nullptr || descriptor->run_multiple_synths != nullptr)
            {
                snd_seq_event_t midiEvents[2];
                carla_zeroStruct<snd_seq_event_t>(midiEvents, 2);

                const unsigned long midiEventCount = 2;

                midiEvents[0].type = SND_SEQ_EVENT_NOTEON;
                midiEvents[0].data.note.note     = 64;
                midiEvents[0].data.note.velocity = 100;

                midiEvents[1].type = SND_SEQ_EVENT_NOTEOFF;
                midiEvents[1].data.note.note     = 64;
                midiEvents[1].data.note.velocity = 0;
                midiEvents[1].time.tick = kBufferSize/2;

                if (descriptor->run_multiple_synths != nullptr && descriptor->run_synth == nullptr)
                {
                    LADSPA_Handle handlePtr[1] = { handle };
                    snd_seq_event_t* midiEventsPtr[1] = { midiEvents };
                    unsigned long midiEventCountPtr[1] = { midiEventCount };
                    descriptor->run_multiple_synths(1, handlePtr, kBufferSize, midiEventsPtr, midiEventCountPtr);
                }
                else
                    descriptor->run_synth(handle, kBufferSize, midiEvents, midiEventCount);
            }
            else
                ldescriptor->run(handle, kBufferSize);

            if (ldescriptor->deactivate != nullptr)
                ldescriptor->deactivate(handle);

            ldescriptor->cleanup(handle);

            // end crash-free plugin test
            // -----------------------------------------------------------------------
        }

        DISCOVERY_OUT("init", "-----------");
        DISCOVERY_OUT("name", ldescriptor->Name);
        DISCOVERY_OUT("label", ldescriptor->Label);
        DISCOVERY_OUT("maker", ldescriptor->Maker);
        DISCOVERY_OUT("copyright", ldescriptor->Copyright);
        DISCOVERY_OUT("uniqueId", ldescriptor->UniqueID);
        DISCOVERY_OUT("hints", hints);
        DISCOVERY_OUT("audio.ins", audioIns);
        DISCOVERY_OUT("audio.outs", audioOuts);
        DISCOVERY_OUT("midi.ins", midiIns);
        DISCOVERY_OUT("parameters.ins", parametersIns);
        DISCOVERY_OUT("parameters.outs", parametersOuts);
        DISCOVERY_OUT("programs", programs);
        DISCOVERY_OUT("build", BINARY_NATIVE);
        DISCOVERY_OUT("end", "------------");
    }
#else
    DISCOVERY_OUT("error", "DSSI support not available");
    return;

    // unused
    (void)libHandle;
    (void)filename;
    (void)init;
#endif
}

static void do_lv2_check(const char* const bundle, const bool init)
{
#ifdef WANT_LV2
    Lv2WorldClass& lv2World(Lv2WorldClass::getInstance());

    // Convert bundle filename to URI
    QString qBundle(QUrl::fromLocalFile(bundle).toString());
    if (! qBundle.endsWith(OS_SEP_STR))
        qBundle += OS_SEP_STR;

    // Load bundle
    Lilv::Node lilvBundle(lv2World.new_uri(qBundle.toUtf8().constData()));
    lv2World.load_bundle(lilvBundle);

    // Load plugins in this bundle
    const Lilv::Plugins lilvPlugins(lv2World.get_all_plugins());

    // Get all plugin URIs in this bundle
    QStringList URIs;

    LILV_FOREACH(plugins, it, lilvPlugins)
    {
        Lilv::Plugin lilvPlugin(lilv_plugins_get(lilvPlugins, it));

        if (const char* const uri = lilvPlugin.get_uri().as_string())
            URIs.append(QString(uri));
    }

    if (URIs.count() == 0)
    {
        DISCOVERY_OUT("warning", "LV2 Bundle doesn't provide any plugins");
        return;
    }

    // Get & check every plugin-instance
    for (int i=0, count=URIs.count(); i < count; ++i)
    {
        const LV2_RDF_Descriptor* const rdfDescriptor(lv2_rdf_new(URIs.at(i).toUtf8().constData(), false));

        if (rdfDescriptor == nullptr || rdfDescriptor->URI == nullptr)
        {
            DISCOVERY_OUT("error", "Failed to find LV2 plugin '" << URIs.at(i).toUtf8().constData() << "'");
            continue;
        }

        if (init)
        {
            // test if DLL is loadable, twice
            void* const libHandle1 = lib_open(rdfDescriptor->Binary);

            if (libHandle1 == nullptr)
            {
                print_lib_error(rdfDescriptor->Binary);
                delete rdfDescriptor;
                continue;
            }

            lib_close(libHandle1);

            void* const libHandle2 = lib_open(rdfDescriptor->Binary);

            if (libHandle2 == nullptr)
            {
                print_lib_error(rdfDescriptor->Binary);
                delete rdfDescriptor;
                continue;
            }

            lib_close(libHandle2);
        }

        // test if we support all required ports and features
        {
            bool supported = true;

            for (uint32_t j=0; j < rdfDescriptor->PortCount && supported; ++j)
            {
                const LV2_RDF_Port* const rdfPort(&rdfDescriptor->Ports[j]);

                if (is_lv2_port_supported(rdfPort->Types))
                {
                    pass();
                }
                else if (! LV2_IS_PORT_OPTIONAL(rdfPort->Properties))
                {
                    DISCOVERY_OUT("error", "Plugin '" << rdfDescriptor->URI << "' requires a non-supported port type (portName: '" << rdfPort->Name << "')");
                    supported = false;
                    break;
                }
            }

            for (uint32_t j=0; j < rdfDescriptor->FeatureCount && supported; ++j)
            {
                const LV2_RDF_Feature* const rdfFeature(&rdfDescriptor->Features[j]);

                if (is_lv2_feature_supported(rdfFeature->URI))
                {
                    pass();
                }
                else if (LV2_IS_FEATURE_REQUIRED(rdfFeature->Type))
                {
                    DISCOVERY_OUT("error", "Plugin '" << rdfDescriptor->URI << "' requires a non-supported feature '" << rdfFeature->URI << "'");
                    supported = false;
                    break;
                }
            }

            if (! supported)
            {
                delete rdfDescriptor;
                continue;
            }
        }

        uint hints = 0x0;
        int audioIns = 0;
        int audioOuts = 0;
        int midiIns = 0;
        int midiOuts = 0;
        int parametersIns = 0;
        int parametersOuts = 0;
        uint programs = rdfDescriptor->PresetCount;

        for (uint32_t j=0; j < rdfDescriptor->FeatureCount; ++j)
        {
            const LV2_RDF_Feature* const rdfFeature(&rdfDescriptor->Features[j]);

            if (std::strcmp(rdfFeature->URI, LV2_CORE__hardRTCapable) == 0)
                hints |= PLUGIN_IS_RTSAFE;
        }

        for (uint32_t j=0; j < rdfDescriptor->PortCount; ++j)
        {
            const LV2_RDF_Port* const rdfPort(&rdfDescriptor->Ports[j]);

            if (LV2_IS_PORT_AUDIO(rdfPort->Types))
            {
                if (LV2_IS_PORT_INPUT(rdfPort->Types))
                    audioIns += 1;
                else if (LV2_IS_PORT_OUTPUT(rdfPort->Types))
                    audioOuts += 1;
            }
            else if (LV2_IS_PORT_CONTROL(rdfPort->Types))
            {
                if (LV2_IS_PORT_DESIGNATION_LATENCY(rdfPort->Designation))
                {
                    pass();
                }
                else if (LV2_IS_PORT_DESIGNATION_SAMPLE_RATE(rdfPort->Designation))
                {
                    pass();
                }
                else if (LV2_IS_PORT_DESIGNATION_FREEWHEELING(rdfPort->Designation))
                {
                    pass();
                }
                else if (LV2_IS_PORT_DESIGNATION_TIME(rdfPort->Designation))
                {
                    pass();
                }
                else
                {
                    if (LV2_IS_PORT_INPUT(rdfPort->Types))
                        parametersIns += 1;
                    else if (LV2_IS_PORT_OUTPUT(rdfPort->Types))
                        parametersOuts += 1;
                }
            }
            else if (LV2_PORT_SUPPORTS_MIDI_EVENT(rdfPort->Types))
            {
                if (LV2_IS_PORT_INPUT(rdfPort->Types))
                    midiIns += 1;
                else if (LV2_IS_PORT_OUTPUT(rdfPort->Types))
                    midiOuts += 1;
            }
        }

        if (LV2_IS_GENERATOR(rdfDescriptor->Type[0], rdfDescriptor->Type[1]))
            hints |= PLUGIN_IS_SYNTH;

        if (rdfDescriptor->UICount > 0)
            hints |= PLUGIN_HAS_CUSTOM_UI;

        DISCOVERY_OUT("init", "-----------");
        DISCOVERY_OUT("uri", rdfDescriptor->URI);
        if (rdfDescriptor->Name != nullptr)
            DISCOVERY_OUT("name", rdfDescriptor->Name);
        if (rdfDescriptor->Author != nullptr)
            DISCOVERY_OUT("maker", rdfDescriptor->Author);
        if (rdfDescriptor->License != nullptr)
            DISCOVERY_OUT("copyright", rdfDescriptor->License);
        DISCOVERY_OUT("uniqueId", rdfDescriptor->UniqueID);
        DISCOVERY_OUT("hints", hints);
        DISCOVERY_OUT("audio.ins", audioIns);
        DISCOVERY_OUT("audio.outs", audioOuts);
        DISCOVERY_OUT("midi.ins", midiIns);
        DISCOVERY_OUT("midi.outs", midiOuts);
        DISCOVERY_OUT("parameters.ins", parametersIns);
        DISCOVERY_OUT("parameters.outs", parametersOuts);
        DISCOVERY_OUT("programs", programs);
        DISCOVERY_OUT("build", BINARY_NATIVE);
        DISCOVERY_OUT("end", "------------");

        delete rdfDescriptor;
    }
#else
    DISCOVERY_OUT("error", "LV2 support not available");
    return;

    // unused
    (void)bundle;
    (void)init;
#endif
}

static void do_vst_check(void*& libHandle, const bool init)
{
#ifdef WANT_VST
    VST_Function vstFn = (VST_Function)lib_symbol(libHandle, "VSTPluginMain");

    if (vstFn == nullptr)
    {
        vstFn = (VST_Function)lib_symbol(libHandle, "main");

        if (vstFn == nullptr)
        {
            DISCOVERY_OUT("error", "Not a VST plugin");
            return;
        }
    }

    AEffect* const effect = vstFn(vstHostCallback);

    if (effect == nullptr || effect->magic != kEffectMagic)
    {
        DISCOVERY_OUT("error", "Failed to init VST plugin, or VST magic failed");
        return;
    }

    if (effect->uniqueID == 0)
    {
        DISCOVERY_OUT("error", "Plugin doesn't have an Unique ID");
        effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);
        return;
    }

    gVstCurrentUniqueId = effect->uniqueID;

    effect->dispatcher(effect, DECLARE_VST_DEPRECATED(effIdentify), 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, DECLARE_VST_DEPRECATED(effSetBlockSizeAndSampleRate), 0, kBufferSize, nullptr, kSampleRate);
    effect->dispatcher(effect, effSetSampleRate, 0, 0, nullptr, kSampleRate);
    effect->dispatcher(effect, effSetBlockSize, 0, kBufferSize, nullptr, 0.0f);
    effect->dispatcher(effect, effSetProcessPrecision, 0, kVstProcessPrecision32, nullptr, 0.0f);

    effect->dispatcher(effect, effOpen, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, effSetProgram, 0, 0, nullptr, 0.0f);

    char strBuf[STR_MAX+1];
    CarlaString cName;
    CarlaString cProduct;
    CarlaString cVendor;

    const intptr_t vstCategory = effect->dispatcher(effect, effGetPlugCategory, 0, 0, nullptr, 0.0f);

    //for (int32_t i = effect->numInputs;  --i >= 0;) effect->dispatcher(effect, DECLARE_VST_DEPRECATED(effConnectInput),  i, 1, 0, 0);
    //for (int32_t i = effect->numOutputs; --i >= 0;) effect->dispatcher(effect, DECLARE_VST_DEPRECATED(effConnectOutput), i, 1, 0, 0);

    carla_zeroChar(strBuf, STR_MAX+1);

    if (effect->dispatcher(effect, effGetVendorString, 0, 0, strBuf, 0.0f) == 1)
        cVendor = strBuf;

    carla_zeroChar(strBuf, STR_MAX+1);

    if (vstCategory == kPlugCategShell)
    {
        gVstCurrentUniqueId = effect->dispatcher(effect, effShellGetNextPlugin, 0, 0, strBuf, 0.0f);

        CARLA_SAFE_ASSERT_RETURN(gVstCurrentUniqueId != 0,);
        cName = strBuf;
    }
    else
    {
        if (effect->dispatcher(effect, effGetEffectName, 0, 0, strBuf, 0.0f) == 1)
            cName = strBuf;
    }

    for (;;)
    {
        carla_zeroChar(strBuf, STR_MAX+1);

        if (effect->dispatcher(effect, effGetProductString, 0, 0, strBuf, 0.0f) == 1)
            cProduct = strBuf;
        else
            cProduct.clear();

        uint hints = 0x0;
        int audioIns = effect->numInputs;
        int audioOuts = effect->numOutputs;
        int midiIns = 0;
        int midiOuts = 0;
        int parameters = effect->numParams;
        int programs = effect->numPrograms;

        if (effect->flags & effFlagsHasEditor)
            hints |= PLUGIN_HAS_CUSTOM_UI;

        if (effect->flags & effFlagsIsSynth)
            hints |= PLUGIN_IS_SYNTH;

        if (vstPluginCanDo(effect, "receiveVstEvents") || vstPluginCanDo(effect, "receiveVstMidiEvent") || (effect->flags & effFlagsIsSynth) != 0)
            midiIns = 1;

        if (vstPluginCanDo(effect, "sendVstEvents") || vstPluginCanDo(effect, "sendVstMidiEvent"))
            midiOuts = 1;

        // -----------------------------------------------------------------------
        // start crash-free plugin test

        if (init)
        {
            if (gVstNeedsIdle)
                effect->dispatcher(effect, DECLARE_VST_DEPRECATED(effIdle), 0, 0, nullptr, 0.0f);

            effect->dispatcher(effect, effMainsChanged, 0, 1, nullptr, 0.0f);
            effect->dispatcher(effect, effStartProcess, 0, 0, nullptr, 0.0f);

            if (gVstNeedsIdle)
                effect->dispatcher(effect, DECLARE_VST_DEPRECATED(effIdle), 0, 0, nullptr, 0.0f);

            // Plugin might call wantMidi() during resume
            if (midiIns == 0 && gVstWantsMidi)
            {
                midiIns = 1;
            }

            float* bufferAudioIn[audioIns];
            for (int j=0; j < audioIns; ++j)
            {
                bufferAudioIn[j] = new float[kBufferSize];
                carla_zeroFloat(bufferAudioIn[j], kBufferSize);
            }

            float* bufferAudioOut[audioOuts];
            for (int j=0; j < audioOuts; ++j)
            {
                bufferAudioOut[j] = new float[kBufferSize];
                carla_zeroFloat(bufferAudioOut[j], kBufferSize);
            }

            struct VstEventsFixed {
                int32_t numEvents;
                intptr_t reserved;
                VstEvent* data[2];

                VstEventsFixed()
                    : numEvents(0),
                      reserved(0)
                {
                    data[0] = data[1] = nullptr;
                }
            } events;

            VstMidiEvent midiEvents[2];
            carla_zeroStruct<VstMidiEvent>(midiEvents, 2);

            midiEvents[0].type = kVstMidiType;
            midiEvents[0].byteSize = sizeof(VstMidiEvent);
            midiEvents[0].midiData[0] = char(MIDI_STATUS_NOTE_ON);
            midiEvents[0].midiData[1] = 64;
            midiEvents[0].midiData[2] = 100;

            midiEvents[1].type = kVstMidiType;
            midiEvents[1].byteSize = sizeof(VstMidiEvent);
            midiEvents[1].midiData[0] = char(MIDI_STATUS_NOTE_OFF);
            midiEvents[1].midiData[1] = 64;
            midiEvents[1].deltaFrames = kBufferSize/2;

            events.numEvents = 2;
            events.data[0] = (VstEvent*)&midiEvents[0];
            events.data[1] = (VstEvent*)&midiEvents[1];

            // processing
            gVstIsProcessing = true;

            if (midiIns > 0)
                effect->dispatcher(effect, effProcessEvents, 0, 0, &events, 0.0f);

            if ((effect->flags & effFlagsCanReplacing) > 0 && effect->processReplacing != nullptr && effect->processReplacing != effect->DECLARE_VST_DEPRECATED(process))
                effect->processReplacing(effect, bufferAudioIn, bufferAudioOut, kBufferSize);
            else if (effect->DECLARE_VST_DEPRECATED(process) != nullptr)
                effect->DECLARE_VST_DEPRECATED(process)(effect, bufferAudioIn, bufferAudioOut, kBufferSize);
            else
                DISCOVERY_OUT("error", "Plugin doesn't have a process function");

            gVstIsProcessing = false;

            effect->dispatcher(effect, effStopProcess, 0, 0, nullptr, 0.0f);
            effect->dispatcher(effect, effMainsChanged, 0, 0, nullptr, 0.0f);

            if (gVstNeedsIdle)
                effect->dispatcher(effect, DECLARE_VST_DEPRECATED(effIdle), 0, 0, nullptr, 0.0f);

            for (int j=0; j < audioIns; ++j)
                delete[] bufferAudioIn[j];
            for (int j=0; j < audioOuts; ++j)
                delete[] bufferAudioOut[j];
        }

        // end crash-free plugin test
        // -----------------------------------------------------------------------

        DISCOVERY_OUT("init", "-----------");
        DISCOVERY_OUT("name", (const char*)cName);
        DISCOVERY_OUT("label", (const char*)cProduct);
        DISCOVERY_OUT("maker", (const char*)cVendor);
        DISCOVERY_OUT("copyright", (const char*)cVendor);
        DISCOVERY_OUT("uniqueId", gVstCurrentUniqueId);
        DISCOVERY_OUT("hints", hints);
        DISCOVERY_OUT("audio.ins", audioIns);
        DISCOVERY_OUT("audio.outs", audioOuts);
        DISCOVERY_OUT("midi.ins", midiIns);
        DISCOVERY_OUT("midi.outs", midiOuts);
        DISCOVERY_OUT("parameters.ins", parameters);
        DISCOVERY_OUT("programs", programs);
        DISCOVERY_OUT("build", BINARY_NATIVE);
        DISCOVERY_OUT("end", "------------");

        if (vstCategory != kPlugCategShell)
            break;

        gVstWantsMidi = false;
        gVstWantsTime = false;

        carla_zeroChar(strBuf, STR_MAX+1);

        gVstCurrentUniqueId = effect->dispatcher(effect, effShellGetNextPlugin, 0, 0, strBuf, 0.0f);

        if (gVstCurrentUniqueId != 0)
            cName = strBuf;
        else
            break;
    }

    if (gVstNeedsIdle)
        effect->dispatcher(effect, DECLARE_VST_DEPRECATED(effIdle), 0, 0, nullptr, 0.0f);

    effect->dispatcher(effect, effClose, 0, 0, nullptr, 0.0f);

#else
    DISCOVERY_OUT("error", "VST support not available");
    return;

    // unused
    (void)libHandle;
    (void)init;
#endif
}

#ifdef HAVE_JUCE
static void do_juce_check(const char* const filename, const char* const stype, const bool init)
{
    using namespace juce;

    ScopedPointer<AudioPluginFormat> pluginFormat;

    if (stype == nullptr)
        return;

    else if (std::strcmp(stype, "LADSPA") == 0)
    {
#if defined(WANT_LADSPA) && JUCE_PLUGINHOST_LADSPA && defined(JUCE_LINUX)
        pluginFormat = new LADSPAPluginFormat();
#else
        DISCOVERY_OUT("error", "LADSPA support not available");
#endif
    }
    /*else if (std::strcmp(stype, "VST") == 0)
    {
#if defined(WANT_VST) && JUCE_PLUGINHOST_VST
        pluginFormat = new VSTPluginFormat();
#else
        DISCOVERY_OUT("error", "VST support not available");
#endif
    }*/
    else if (std::strcmp(stype, "VST3") == 0)
    {
#if defined(WANT_VST3) && JUCE_PLUGINHOST_VST3
        pluginFormat = new VSTPluginFormat();
#else
        DISCOVERY_OUT("error", "VST3 support not available");
#endif
    }
    else if (std::strcmp(stype, "AU") == 0)
    {
#if defined(WANT_AU) && JUCE_PLUGINHOST_AU && defined(JUCE_MAC)
        pluginFormat = new AudioUnitPluginFormat();
#else
        DISCOVERY_OUT("error", "AU support not available");
#endif
    }

    if (pluginFormat == nullptr)
    {
        DISCOVERY_OUT("error", stype << " support not available");
        return;
    }

    OwnedArray<PluginDescription> results;
    pluginFormat->findAllTypesForFile(results, filename);

    for (PluginDescription **it = results.begin(), **end = results.end(); it != end; ++it)
    {
        static int iv=0;
        carla_stderr2("LOOKING FOR PLUGIN %i", iv++);
        PluginDescription* const desc(*it);

        uint hints = 0x0;
        int audioIns = desc->numInputChannels;
        int audioOuts = desc->numOutputChannels;
        int midiIns = 0;
        int midiOuts = 0;
        int parameters = 0;
        int programs = 0;

        if (desc->isInstrument)
            hints |= PLUGIN_IS_SYNTH;

        if (init)
        {
            if (AudioPluginInstance* const instance = pluginFormat->createInstanceFromDescription(*desc, kSampleRate, kBufferSize))
            {
                instance->refreshParameterList();

                parameters = instance->getNumParameters();
                programs   = instance->getNumPrograms();

                if (instance->hasEditor())
                    hints |= PLUGIN_HAS_CUSTOM_UI;
                if (instance->acceptsMidi())
                    midiIns = 1;
                if (instance->producesMidi())
                    midiOuts = 1;

                delete instance;
            }
        }

        DISCOVERY_OUT("init", "-----------");
        DISCOVERY_OUT("name", desc->name);
        DISCOVERY_OUT("label", desc->descriptiveName);
        DISCOVERY_OUT("maker", desc->manufacturerName);
        DISCOVERY_OUT("copyright", desc->manufacturerName);
        DISCOVERY_OUT("uniqueId", desc->uid);
        DISCOVERY_OUT("hints", hints);
        DISCOVERY_OUT("audio.ins", audioIns);
        DISCOVERY_OUT("audio.outs", audioOuts);
        DISCOVERY_OUT("midi.ins", midiIns);
        DISCOVERY_OUT("midi.outs", midiOuts);
        DISCOVERY_OUT("parameters.ins", parameters);
        DISCOVERY_OUT("programs", programs);
        DISCOVERY_OUT("build", BINARY_NATIVE);
        DISCOVERY_OUT("end", "------------");
    }
}
#endif

static void do_csound_check(const char* const filename, const bool init)
{
#ifdef WANT_CSOUND
    Csound csound;
# ifndef DEBUG
    csound.SetMessageCallback(csound_silence);
# endif
    csound.SetHostImplementedAudioIO(true, kBufferSize);
    csound.SetHostImplementedMIDIIO(true);
    csound.Reset();

    csound.SetExternalMidiInOpenCallback(csound_midiInOpen);
    csound.SetExternalMidiReadCallback(csound_midiRead);
    csound.SetExternalMidiInCloseCallback(csound_midiInClose);

    csound.SetExternalMidiOutOpenCallback(csound_midiOutOpen);
    csound.SetExternalMidiWriteCallback(csound_midiWrite);
    csound.SetExternalMidiOutCloseCallback(csound_midiOutClose);

    if (csound.Compile(const_cast<char*>(filename)) != 0)
    {
        DISCOVERY_OUT("error", "csound failed to compile");
        return;
    }

    csound.PerformKsmps();
    csound.SetScoreOffsetSeconds(0);
    csound.RewindScore();

    int hints = 0x0;
    int audioIns = 0;
    int audioOuts = 0;
    int midiIns = 0;
    int midiOuts = 0;
    int parametersIns = 0;
    int parametersOuts = 0;
    int programs = 0;

    int numChannels;
    controlChannelInfo_t* channelList;

    numChannels = csound.ListChannels(channelList);

    carla_stderr2("Num chan %i", numChannels);

    if (numChannels != 0 && channelList != nullptr)
    {
        for (int i=0; i < numChannels; ++i)
        {
            const controlChannelInfo_t& channel(channelList[i]);

            carla_stderr2("chan @%i, type %i", i, channel.type);

            if (channel.type & CSOUND_AUDIO_CHANNEL)
            {
                if (channel.type & CSOUND_INPUT_CHANNEL)
                    audioIns += 1;
                else if (channel.type & CSOUND_OUTPUT_CHANNEL)
                    audioOuts += 1;
            }
            else if (channel.type & CSOUND_CONTROL_CHANNEL)
            {
                if (channel.type & CSOUND_INPUT_CHANNEL)
                    parametersIns += 1;
                else if (channel.type & CSOUND_OUTPUT_CHANNEL)
                    parametersOuts += 1;
            }
        }

        csound.DeleteChannelList(channelList);
    }

    // TODO

    csound.Cleanup();
    csound.Reset();

    DISCOVERY_OUT("init", "-----------");
//     DISCOVERY_OUT("name", (const char*)name);
//     DISCOVERY_OUT("label", (const char*)label);
//     DISCOVERY_OUT("maker", "");
//     DISCOVERY_OUT("copyright", "");
    DISCOVERY_OUT("hints", hints);
    DISCOVERY_OUT("audio.ins", audioIns);
    DISCOVERY_OUT("audio.outs", audioOuts);
    DISCOVERY_OUT("midi.ins", midiIns);
    DISCOVERY_OUT("midi.outs", midiOuts);
    DISCOVERY_OUT("parameters.ins", parametersIns);
    DISCOVERY_OUT("parameters.outs", parametersOuts);
    DISCOVERY_OUT("programs", programs);
    DISCOVERY_OUT("build", BINARY_NATIVE);
    DISCOVERY_OUT("end", "------------");

#else
    DISCOVERY_OUT("error", "csound support not available");
    return;

    // unused
    (void)filename;
    (void)init;
#endif
}

static void do_fluidsynth_check(const char* const filename, const bool init)
{
#ifdef WANT_FLUIDSYNTH
    if (! fluid_is_soundfont(filename))
    {
        DISCOVERY_OUT("error", "Not a SF2 file");
        return;
    }

    int programs = 0;

    if (init)
    {
        fluid_settings_t* const f_settings = new_fluid_settings();
        fluid_synth_t* const f_synth = new_fluid_synth(f_settings);
        const int f_id = fluid_synth_sfload(f_synth, filename, 0);

        if (f_id < 0)
        {
            DISCOVERY_OUT("error", "Failed to load SF2 file");
            return;
        }

        fluid_sfont_t* f_sfont;
        fluid_preset_t f_preset;

        f_sfont = fluid_synth_get_sfont_by_id(f_synth, static_cast<uint>(f_id));

        f_sfont->iteration_start(f_sfont);
        while (f_sfont->iteration_next(f_sfont, &f_preset))
            programs += 1;

        delete_fluid_synth(f_synth);
        delete_fluid_settings(f_settings);
    }

    CarlaString name;

    if (const char* const shortname = std::strrchr(filename, OS_SEP))
        name = shortname+1;
    else
        name = filename;

    name.truncate(name.rfind('.'));

    CarlaString label(name);

    // 2 channels
    DISCOVERY_OUT("init", "-----------");
    DISCOVERY_OUT("name", (const char*)name);
    DISCOVERY_OUT("label", (const char*)label);
    DISCOVERY_OUT("hints", PLUGIN_IS_SYNTH);
    DISCOVERY_OUT("audio.outs", 2);
    DISCOVERY_OUT("midi.ins", 1);
    DISCOVERY_OUT("programs", programs);
    DISCOVERY_OUT("parameters.ins", 13); // defined in Carla
    DISCOVERY_OUT("parameters.outs", 1);
    DISCOVERY_OUT("build", BINARY_NATIVE);
    DISCOVERY_OUT("end", "------------");

    // 16 channels
    if (name.isEmpty() || programs <= 1)
        return;
    name += " (16 outputs)";

    DISCOVERY_OUT("init", "-----------");
    DISCOVERY_OUT("name", (const char*)name);
    DISCOVERY_OUT("label", (const char*)label);
    DISCOVERY_OUT("hints", PLUGIN_IS_SYNTH);
    DISCOVERY_OUT("audio.outs", 32);
    DISCOVERY_OUT("midi.ins", 1);
    DISCOVERY_OUT("programs", programs);
    DISCOVERY_OUT("parameters.ins", 13); // defined in Carla
    DISCOVERY_OUT("parameters.outs", 1);
    DISCOVERY_OUT("build", BINARY_NATIVE);
    DISCOVERY_OUT("end", "------------");
#else
    DISCOVERY_OUT("error", "SF2 support not available");
    return;

    // unused
    (void)filename;
    (void)init;
#endif
}

static void do_linuxsampler_check(const char* const filename, const char* const stype, const bool init)
{
#ifdef WANT_LINUXSAMPLER
    const QFileInfo file(filename);

    if (! file.exists())
    {
        DISCOVERY_OUT("error", "Requested file does not exist");
        return;
    }

    if (! file.isFile())
    {
        DISCOVERY_OUT("error", "Requested file is not valid");
        return;
    }

    if (! file.isReadable())
    {
        DISCOVERY_OUT("error", "Requested file is not readable");
        return;
    }

    if (init)
        const LinuxSamplerScopedEngine engine(filename, stype);
    else
        LinuxSamplerScopedEngine::outputInfo(nullptr, 0, file.baseName().toUtf8().constData());
#else
    DISCOVERY_OUT("error", stype << " support not available");
    return;

    // unused
    (void)filename;
    (void)init;
#endif
}

// --------------------------------------------------------------------------

class ScopedWorkingDirSet
{
public:
    ScopedWorkingDirSet(const char* const filename)
        : fPreviousPath(QDir::currentPath())
    {
        QDir dir(filename);
        dir.cdUp();
        QDir::setCurrent(dir.absolutePath());
    }

    ~ScopedWorkingDirSet()
    {
        QDir::setCurrent(fPreviousPath);
    }

private:
    const QString fPreviousPath;
};

// ------------------------------ main entry point ------------------------------

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        carla_stdout("usage: %s <type> </path/to/plugin>", argv[0]);
        return 1;
    }

    const char* const stype    = argv[1];
    const char* const filename = argv[2];
    const PluginType  type     = getPluginTypeFromString(stype);

    const ScopedWorkingDirSet swds(filename);

    CarlaString filenameStr(filename);
    filenameStr.toLower();

    if (filenameStr.contains("fluidsynth", true))
    {
        DISCOVERY_OUT("info", "skipping fluidsynth based plugin");
        return 0;
    }
    if (filenameStr.contains("linuxsampler", true) || filenameStr.endsWith("ls16.so"))
    {
        DISCOVERY_OUT("info", "skipping linuxsampler based plugin");
        return 0;
    }

    bool openLib = false;
    void* handle = nullptr;

    switch (type)
    {
    case PLUGIN_LADSPA:
    case PLUGIN_DSSI:
    case PLUGIN_VST:
        openLib = true;
    default:
        break;
    }

    if (openLib)
    {
        handle = lib_open(filename);

        if (handle == nullptr)
        {
            print_lib_error(filename);
            return 1;
        }
    }

    // never do init for dssi-vst, takes too long and it's crashy
    bool doInit = ! filenameStr.contains("dssi-vst", true);

    if (doInit && getenv("CARLA_DISCOVERY_NO_PROCESSING_CHECKS") != nullptr)
        doInit = false;

    if (doInit && handle != nullptr)
    {
        // test fast loading & unloading DLL without initializing the plugin(s)
        if (! lib_close(handle))
        {
            print_lib_error(filename);
            return 1;
        }

        handle = lib_open(filename);

        if (handle == nullptr)
        {
            print_lib_error(filename);
            return 1;
        }
    }

    switch (type)
    {
    case PLUGIN_LADSPA:
        do_ladspa_check(handle, filename, doInit);
        break;
    case PLUGIN_DSSI:
        do_dssi_check(handle, filename, doInit);
        break;
    case PLUGIN_LV2:
        do_lv2_check(filename, doInit);
        break;
    case PLUGIN_VST:
        do_vst_check(handle, doInit);
        break;
    case PLUGIN_VST3:
#ifdef HAVE_JUCE
        do_juce_check(filename, "VST3", doInit);
#else
        DISCOVERY_OUT("error", "VST3 support not available");
#endif
        break;
    case PLUGIN_AU:
#ifdef HAVE_JUCE
        do_juce_check(filename, "AU", doInit);
#else
        DISCOVERY_OUT("error", "AU support not available");
#endif
        break;
    case PLUGIN_FILE_CSD:
        do_csound_check(filename, doInit);
        break;
    case PLUGIN_FILE_GIG:
        do_linuxsampler_check(filename, "gig", doInit);
        break;
    case PLUGIN_FILE_SF2:
        do_fluidsynth_check(filename, doInit);
        break;
    case PLUGIN_FILE_SFZ:
        do_linuxsampler_check(filename, "sfz", doInit);
        break;
    default:
        break;
    }

    if (openLib && handle != nullptr)
        lib_close(handle);

    return 0;
}

// --------------------------------------------------------------------------
// Extras

#ifdef WANT_DSSI
# include "CarlaDssiUtils.cpp"
#endif

#ifdef HAVE_JUCE
// --------------------------------------------------------------------------
// we want juce_audio_processors but without UI code
// this is copied from juce_audio_processors.cpp

#include "juce_core/native/juce_BasicNativeHeaders.h"

namespace juce
{

static inline
bool arrayContainsPlugin(const OwnedArray<PluginDescription>& list, const PluginDescription& desc)
{
    for (int i = list.size(); --i >= 0;)
    {
        if (list.getUnchecked(i)->isDuplicateOf(desc))
            return true;
    }
    return false;
}

#include "juce_audio_processors/format/juce_AudioPluginFormat.cpp"
#include "juce_audio_processors/processors/juce_AudioProcessor.cpp"
#include "juce_audio_processors/processors/juce_PluginDescription.cpp"

#ifdef WANT_LADSPA
# include "juce_audio_processors/format_types/juce_LADSPAPluginFormat.cpp"
#endif
#if 0 //def WANT_VST
# include "juce_audio_processors/format_types/juce_VSTPluginFormat.cpp"
#endif
#ifdef WANT_VST3
# include "juce_audio_processors/format_types/juce_VST3PluginFormat.cpp"
#endif
#ifdef WANT_AU
# include "juce_audio_processors/format_types/juce_AudioUnitPluginFormat.mm"
#endif
}
#endif

// --------------------------------------------------------------------------
