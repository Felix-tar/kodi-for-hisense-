set(SOURCES TVAudioTranscoder.cpp
            TVBridgeIO.cpp
            TVEsEngine.cpp
            TVPlayer.cpp
            TVPlayerBridge.cpp
            TVPlayerHttpFeeder.cpp
            TVPlayerProbe.cpp)

set(HEADERS TVAudioTranscoder.h
            TVBridgeIO.h
            TVEsEngine.h
            TVEsProtocol.h
            TVPlayer.h
            TVPlayerBridge.h
            TVPlayerHttpFeeder.h
            TVPlayerProbe.h)

core_add_library(tvplayer)
