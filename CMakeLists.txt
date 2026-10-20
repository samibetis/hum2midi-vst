cmake_minimum_required(VERSION 3.22)
project(Hum2Midi VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Descarga JUCE automáticamente la primera vez que configuras (necesita internet).
include(FetchContent)
FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.4.zip)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(Hum2Midi
    COMPANY_NAME "samibetis"
    PLUGIN_MANUFACTURER_CODE Smbt        # 4 letras, 1 mayúscula mínimo
    PLUGIN_CODE Hm2m                     # 4 letras, identifica el plugin
    PRODUCT_NAME "hum2midi"
    FORMATS VST3 Standalone              # Standalone = .exe para probar sin FL
    IS_SYNTH FALSE                       # es un efecto: se pone en una pista del mixer
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT TRUE               # además del synth interno, saca MIDI
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE        # lo copias tú a la carpeta VST3 (evita pedir admin)
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "hum2midi necesita el micro para escuchar lo que tarareas."
    VST3_CATEGORIES Fx Analyzer)

target_sources(Hum2Midi PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Hum2Midi PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

# Los textos de la interfaz llevan tildes: le decimos a Visual Studio que el código es UTF-8.
target_compile_options(Hum2Midi PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_link_libraries(Hum2Midi
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
