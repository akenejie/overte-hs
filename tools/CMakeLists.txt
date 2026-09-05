# Headless project: no build tools are compiled (OVERTE_BUILD_TOOLS is forced
# OFF). tools/shadergen.py is still referenced at build time by
# AutoScribeShader.cmake for the `shaders` library, so it stays in the tree.
# This stub keeps the top-level `add_subdirectory(tools)` valid.
add_custom_target(tools SOURCES shadergen.py)