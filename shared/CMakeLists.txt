oxymp_library(shared
    SOURCES
        src/messages.cpp
        src/mvalue.cpp
        src/mvalue_codec.cpp
        src/serialization.cpp
        src/vault.cpp
    INCLUDES include
    PRIVATE oxymp::options
)
