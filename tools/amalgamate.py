#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Amalgamate the modular ember sources into a single, readable header.

Usage:
    python tools/amalgamate.py                # write single_header/ember.hpp
    python tools/amalgamate.py --verify       # regenerate in memory, fail on drift

The generated file keeps the documented external dependencies (glad, glm,
GLFW/stb_image optional) — see the header banner and the INTEGRATION docs.
Run after any change to include/ember/*.hpp or src/*.cpp.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "single_header", "ember.hpp")

# ---------------------------------------------------------------------------
# Section order (dependency order) + one-line descriptions for the banner TOC.
# ---------------------------------------------------------------------------
DECL_ORDER = [
    "gl.hpp",               # must come first (GL types/helpers for everything else)
    "core.hpp",
    "emitters.hpp",
    "gpu.hpp",
    "shader.hpp",
    "config.hpp",
    "particle_system.hpp",
    # glfw_window.hpp is appended under #ifdef EMBER_USE_GLFW
]

DECL_DESC = {
    "gl.hpp":               "GL loading + error helpers (glad required)",
    "core.hpp":             "Particle, force types, Rng",
    "emitters.hpp":         "Emitter + SpawnRequest (GPU spawn requests)",
    "gpu.hpp":              "Buffer / Texture / VertexArray RAII wrappers",
    "shader.hpp":           "Shader — uniform-cached GL program",
    "config.hpp":           "INI-style Config parser (loadConfig/apply)",
    "particle_system.hpp":  "ParticleSystem — GPU simulation + rendering",
    "glfw_window.hpp":      "GLFW convenience window (EMBER_USE_GLFW)",
}

IMPL_ORDER = [
    "shader.cpp",
    "particle_system.cpp",  # preceded by the conditional stb_image include
    # glfw_window.cpp is appended under #ifdef EMBER_USE_GLFW
]

IMPL_DESC = {
    "shader.cpp":           "Shader implementation",
    "particle_system.cpp":  "ParticleSystem + embedded shader sources",
    "glfw_window.cpp":      "GLFW window wrapper (EMBER_USE_GLFW)",
}

INTERNAL_INCLUDE = re.compile(r'^\s*#\s*include\s+"ember/[^"]+"\s*$')
STB_INCLUDE = re.compile(r'^\s*#\s*include\s+"stb_image\.h"\s*$')
EXTERNAL_INCLUDE = re.compile(r'^\s*#\s*include\s*<[^>]+>\s*$')

BANNER = r"""/* ============================================================================
 * ember.hpp — ember GPU particle library · single-header edition
 * ============================================================================
 * GENERATED FILE — do not edit by hand. Regenerate with:
 *     python tools/amalgamate.py           (--verify checks for drift)
 * ============================================================================
 * CONTENTS
 *   SECTION A — DECLARATIONS (types, classes, inline helpers)
 *     [ 1] ember/gl.hpp            GL loading + error helpers (needs glad)
 *     [ 2] ember/core.hpp          Particle, force types, Rng
 *     [ 3] ember/emitters.hpp      Emitter + SpawnRequest (GPU spawn requests)
 *     [ 4] ember/gpu.hpp           Buffer / Texture / VertexArray RAII
 *     [ 5] ember/shader.hpp        Shader — uniform-cached GL program
 *     [ 6] ember/config.hpp        INI-style Config parser (loadConfig/apply)
 *     [ 7] ember/particle_system.hpp  ParticleSystem (simulation + rendering)
 *     [ 8] ember/glfw_window.hpp   [EMBER_USE_GLFW] GLFW convenience window
 *   SECTION B — IMPLEMENTATION (compile ONCE, see below)
 *     [ 9] src/shader.cpp
 *     [10] src/particle_system.cpp
 *     [11] src/glfw_window.cpp     [EMBER_USE_GLFW]
 * ============================================================================
 * QUICK START — in exactly ONE translation unit:
 *
 *     #define EMBER_IMPLEMENTATION
 *     #define EMBER_USE_GLFW            // optional: GLFW window module
 *     #define EMBER_USE_STB             // optional: PNG sprites
 *     #include "ember.hpp"
 *
 *     int main() {
 *         ember::Window win(1280, 720, "demo");   // 4.3 core context + gl::init
 *         ember::ParticleSystem sys({100000, 30000});
 *         sys.setGravity({0.f, -9.81f, 0.f});
 *         // main loop: win.pollEvents(); sys.update(dt);
 *         //            sys.render(view, proj, fbW, fbH, fovYDeg);
 *     }
 * ============================================================================
 * FEATURE MACROS
 *     EMBER_IMPLEMENTATION  define in ONE TU to compile the implementation
 *     EMBER_USE_GLFW        include the GLFW window convenience module
 *     EMBER_USE_STB         PNG sprite support (define STB_IMAGE_IMPLEMENTATION
 *                           in some TU as well)
 * ============================================================================
 * DEPENDENCIES (documented in INTEGRATION docs, §2)
 *     glad        OpenGL 4.3 core loader — include <glad/gl.h> + compile glad.c
 *     glm (>=0.9.9) header-only math
 *     GLFW 3.4    only with EMBER_USE_GLFW
 *     stb_image.h only with EMBER_USE_STB
 * ============================================================================
 * LICENSE — MIT (see the LICENSE file in the repository root).
 * ==========================================================================*/
"""

DECL_HEADER = r"""
/* ============================================================================
 * SECTION A — DECLARATIONS
 * ==========================================================================*/
"""

IMPL_HEADER = r"""
/* ============================================================================
 * SECTION B — IMPLEMENTATION
 * Compile ONCE: define EMBER_IMPLEMENTATION before including this header
 * (one TU in the whole project). Without it this section is skipped.
 * ==========================================================================*/
"""

BAR = " * ========================================================================== */"


def banner_for(num, tag, name, desc):
    return ("/* ============================================================================\n"
            " * [%2d] %s\n"
            " * %s\n"
            " * ========================================================================== */\n"
            % (num, name, desc))


def read(name):
    subdir = "include/ember" if name.endswith(".hpp") else "src"
    with open(os.path.join(ROOT, subdir, name), "r", encoding="utf-8") as f:
        return f.read()


def tidy(text):
    """Collapse runs of 3+ blank lines to a single blank line (safe for C++ and
    the embedded GLSL raw strings)."""
    out = []
    blanks = 0
    for line in text.split("\n"):
        if line.strip() == "":
            blanks += 1
            if blanks <= 1:
                out.append("")
        else:
            blanks = 0
            out.append(line)
    while out and out[-1] == "":
        out.pop()
    return "\n".join(out)


def strip(name, text, seen):
    """Remove single-header-internal directives; dedupe repeated external
    includes (first occurrence wins, in place)."""
    out = []
    for line in text.split("\n"):
        s = line.strip()
        if s == "#pragma once":
            continue
        if INTERNAL_INCLUDE.match(line) or STB_INCLUDE.match(line):
            continue
        if EXTERNAL_INCLUDE.match(line):
            inc = s
            if inc in seen:
                continue  # already emitted earlier in this section
            seen.add(inc)
        out.append(line)
    return tidy("\n".join(out))


def build():
    lines = [BANNER]

    # ---- SECTION A: declarations -------------------------------------------
    lines.append(DECL_HEADER)
    lines.append("\n#ifndef EMBER_SINGLE_HEADER_HPP\n#define EMBER_SINGLE_HEADER_HPP\n")
    seen = set()
    num = 0
    for name in DECL_ORDER:
        num += 1
        lines.append(banner_for(num, name, name, DECL_DESC[name]))
        lines.append(strip(name, read(name), seen))

    glfw_h = read("glfw_window.hpp")
    num += 1
    lines.append("\n#ifdef EMBER_USE_GLFW\n")
    lines.append(banner_for(num, "glfw_window.hpp", "glfw_window.hpp", DECL_DESC["glfw_window.hpp"]))
    lines.append(strip("glfw_window.hpp", glfw_h, seen))  # its GLFW include stays in place
    lines.append("\n#endif // EMBER_USE_GLFW\n")
    lines.append("\n#endif // EMBER_SINGLE_HEADER_HPP\n")

    # ---- SECTION B: implementation ------------------------------------------
    lines.append(IMPL_HEADER)
    lines.append("\n#ifdef EMBER_IMPLEMENTATION\n#ifndef EMBER_SINGLE_HEADER_IMPLEMENTATION\n"
                 "#define EMBER_SINGLE_HEADER_IMPLEMENTATION\n")
    seen = set()
    for name in IMPL_ORDER:
        num += 1
        body = strip(name, read(name), seen)
        if name == "particle_system.cpp":
            body = ("#ifdef EMBER_USE_STB\n"
                    "#include \"stb_image.h\"\n"
                    "#endif // EMBER_USE_STB\n\n") + body
        lines.append(banner_for(num, name, name, IMPL_DESC[name]))
        lines.append(body)

    glfw_cpp = read("glfw_window.cpp")
    num += 1
    lines.append("\n#ifdef EMBER_USE_GLFW\n")
    lines.append(banner_for(num, "glfw_window.cpp", "glfw_window.cpp", IMPL_DESC["glfw_window.cpp"]))
    lines.append(strip("glfw_window.cpp", glfw_cpp, seen))
    lines.append("\n#endif // EMBER_USE_GLFW\n")
    lines.append("\n#endif // EMBER_SINGLE_HEADER_IMPLEMENTATION\n#endif // EMBER_IMPLEMENTATION\n")
    return tidy("\n".join(lines))


def main():
    verify = "--verify" in sys.argv
    data = build()
    if verify:
        try:
            with open(OUT, "r", encoding="utf-8") as f:
                existing = f.read()
        except FileNotFoundError:
            print("MISSING: %s (run without --verify to generate)" % OUT)
            return 1
        if existing != data:
            print("DRIFT: %s is out of date — run `python tools/amalgamate.py`" % OUT)
            return 1
        print("OK: %s is up to date (%d bytes)" % (OUT, len(data)))
        return 0
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(data)
    print("Wrote %s (%d bytes)" % (OUT, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
