set(SRCS
    simple.vert
    simple.frag)

set(SHADER_INSTALL_DIR ${CMAKE_BINARY_DIR}/example-shaders)

build_shaders()
install_shaders()
