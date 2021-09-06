set(SRCS
    comp2a.frag
    comp3a.frag
    comp4a.frag
    comp.frag
    paint.rchit
    paint.rgen
    paint.rmiss)

include(author_shaders)
author_shaders(dali_shaders dali dali_spvs
    SOURCES ${SRCS})
