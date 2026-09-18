#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Keep the embedded GLSL copies in src/backends/opengl/shaders.cpp in sync with the
canonical shaders/ files (kSimulateComp / kParticleVert / kParticleFrag).

The embedded copies are the zero-file fallback when shaders/ is absent, so they
must match the files exactly. Run after editing any shaders/*.glsl or *.vert/*.frag.
"""
import pathlib
import re
import sys

cpp_path = pathlib.Path("src/backends/opengl/shaders.cpp")
mapping = {
    "kSimulateComp": "shaders/simulate.comp",
    "kParticleVert": "shaders/particle.vert",
    "kParticleFrag": "shaders/particle.frag",
    "kSortComp": "shaders/sort.comp",
    "kScheduleComp": "shaders/schedule.comp",
}


def main():
    verify = "--verify" in sys.argv
    cpp = cpp_path.read_text(encoding="utf-8")
    for name, path in mapping.items():
        src = pathlib.Path(path).read_text(encoding="utf-8")
        pat = re.compile(r'(const char\* const %s = R"GLSL\()(.*?)(\)GLSL";)' % name, re.DOTALL)
        m = pat.search(cpp)
        if not m:
            print("ERROR: embedded %s not found" % name)
            return 1
        if verify:
            if m.group(2) != src:
                print("DRIFT: embedded %s != %s — run `python tools/sync_embedded_shaders.py`" % (name, path))
                return 1
        else:
            cpp = pat.sub(lambda mm: mm.group(1) + src + mm.group(3), cpp, count=1)
            print("synced %s <- %s" % (name, path))
    if not verify:
        cpp_path.write_text(cpp, encoding="utf-8")
    else:
        print("OK: embedded shaders up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
