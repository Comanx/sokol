set -e
source tests/test_common.sh
build ios_gl ios_gl_debug
build ios_gl ios_gl_release
build ios_metal ios_metal_debug
build ios_metal ios_metal_release
build ios_arc_gl ios_arc_gl_debug
build ios_arc_gl ios_arc_gl_release
build ios_arc_metal ios_arc_metal_debug
build ios_arc_metal ios_arc_metal_release
