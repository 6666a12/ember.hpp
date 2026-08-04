#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Amalgamate the modular ember sources into a single header.

Usage:
    python tools/amalgamate.py                # write single_header/ember.hpp
    python tools/amalgamate.py --verify       # regenerate in memory, fail on drift

The generated file keeps the documented external dependencies (glad, glm,
GLFW/stb_image optional) — see the header comment and INTEGRATION docs.
Run after any change to include/ember/*.hpp or src/*.cpp.
"""
import os
import sys
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "single_header", "ember.hpp")

# Declaration sections, in dependency order.
DECL_ORDER = [
    "gl.hpp",              # ember::gl helpers; includes <glad/gl.h>
    "core.hpp",            # Particle / forces / Rng
    "emitters.hpp",        # Emitter / SpawnRequest
    "gpu.hpp",             # Buffer / Texture / VertexArray
    "shader.hpp",          # Shader
    "config.hpp",          # INI-style Config parser
    "particle_system.hpp", # ParticleSystem
    # glfw_window.hpp is appended under #ifdef EMBER_USE_GLFW
]

IMPL_ORDER = [
    "shader.cpp",
    "particle_system.cpp", # preceded by the conditional stb_image include
    # glfw_window.cpp is appended under #ifdef EMBER_USE_GLFW
]

INTERNAL_INCLUDE = re.compile(r'^\s*#\s*include\s+"ember/[^"]+"\s*$')
STB_INCLUDE = re.compile(r'^\s*#\s*include\s+"stb_image\.h"\s*$')


def read(name):
    subdir = "include/ember" if name.endswith(".hpp") else "src"
    with open(os.path.join(ROOT, subdir, name), "r", encoding="utf-8") as f:
        return f.read()


def strip(name, text):
    """Remove single-header-internal directives from a source file."""
    out = []
    for line in text.splitlines():
        s = line.strip()
        if s == "#pragma once":
            continue
        if INTERNAL_INCLUDE.match(line) or STB_INCLUDE.match(line):
            continue
        out.append(line)
    return "\n".join(out) + "\n"


def section(tag, name, body):
    return "// ================= [%s] =================\n%s\n" % (tag, body)


def build():
    lines = []
    lines.append("""// ember.hpp — single-header edition of the ember GPU particle library.
//
// GENERATED FILE — do not edit by hand. Regenerate with:
//     python tools/amalgamate.py
// (after changing include/ember/*.hpp or src/*.cpp)
//
// Usage (one translation unit):
//     #define EMBER_IMPLEMENTATION
//     #include "ember.hpp"
// Optional feature macros:
//     EMBER_USE_GLFW — include the GLFW window convenience module
//     EMBER_USE_STB  — PNG sprite support; define STB_IMAGE_IMPLEMENTATION
//                      somewhere (e.g. in the same TU, before this include)
//
// External dependencies (documented in INTEGRATION docs):
//   * glad  — OpenGL 4.3 core loader; the generated gl.h must be includeable
//             as <glad/gl.h> and its implementation TU (glad.c) linked.
//   * glm   — header-only math library (>= 0.9.9).
//   * GLFW  — only with EMBER_USE_GLFW (window convenience module).
//   * stb_image.h — only with EMBER_USE_STB (PNG sprites); without it
//             setSpriteTexture falls back to the built-in gradient.
//
// License: MIT (see LICENSE).
""")

    lines.append("""
#ifndef EMBER_SINGLE_HEADER_HPP
#define EMBER_SINGLE_HEADER_HPP
""")

    for name in DECL_ORDER:
        lines.append(section("ember/" + name, name, strip(name, read(name))))

    # glfw_window.hpp — optional windowing convenience.
    glfw_h = read("glfw_window.hpp")
    lines.append("""
#ifdef EMBER_USE_GLFW
%s
#endif // EMBER_USE_GLFW
""" % section("ember/glfw_window.hpp", "glfw_window.hpp", strip("glfw_window.hpp", glfw_h)))

    lines.append("""
#endif // EMBER_SINGLE_HEADER_HPP

// =====================================================================
// Implementation — compile ONCE by defining EMBER_IMPLEMENTATION in a
// single translation unit (e.g. right before this include).
// =====================================================================
#ifdef EMBER_IMPLEMENTATION
#ifndef EMBER_SINGLE_HEADER_IMPLEMENTATION
#define EMBER_SINGLE_HEADER_IMPLEMENTATION
""")

    for name in IMPL_ORDER:
        body = strip(name, read(name))
        if name == "particle_system.cpp":
            body = ("#ifdef EMBER_USE_STB\n"
                    "#include \"stb_image.h\"\n"
                    "#endif // EMBER_USE_STB\n\n") + body
        lines.append(section("src/" + name, name, body))

    glfw_cpp = read("glfw_window.cpp")
    lines.append("""
#ifdef EMBER_USE_GLFW
%s
#endif // EMBER_USE_GLFW
""" % section("src/glfw_window.cpp", "glfw_window.cpp", strip("glfw_window.cpp", glfw_cpp)))

    lines.append("""
#endif // EMBER_SINGLE_HEADER_IMPLEMENTATION
#endif // EMBER_IMPLEMENTATION
""")
    return "".join(lines)


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
