"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

------------------------------------------------------------------- why

`demo/plugin.js` holds a copy of every piece of GLSL in `source/Shaders.cpp`.
That is two copies of the same text, and two copies drift -- quietly, because a
demo that renders a *plausible* picture looks exactly like a demo that renders
the right one. The whole claim of these pages is that they run the plugin's own
shader rather than something reimplemented to look similar, so the claim needs
something enforcing it.

Nothing else can. `tools/check-shaders.sh` is a different check: it hands the
shaders `tptest --dump-shaders` writes -- the C++ strings as the plugin
assembles them -- to glslc, to prove they COMPILE. It never looks at the page.
And `tptest` drives the real plugin class and has no idea this page exists.

------------------------------------------------------------------- what it does

Every shader in the plugin is assembled at run time, `assemble( body )`: the
`kVersion` line, then one of ten raw-string bodies. The page carries the pieces
rather than the assembled text -- `VERSION` and the ten `..._BODY` constants,
joined the same way in plugin.js -- so this compares the pieces: all eleven.

Each `R"( ... )"` body is pulled out of the C++ and each matching backtick
literal out of `plugin.js`, and compared exactly -- no whitespace normalisation,
no comment stripping. A comment that has been updated on one side and not the
other is exactly the drift worth catching, because the comments in Shaders.cpp
carry the reasoning: why the flood's distances are squared integers, why the
resolve subtracts half a texel, why the bevel's gradient is taken a quarter of
the band apart and not one texel.

`kVersion` is an ordinary C string, `"#version 410 core\\n"`, so its one escape
is decoded before comparing; on the JS side it is a template literal ending in
a real newline.

The one transformation on the JS side is a decode, not a normalisation. The
composite quotes an identifier in a comment with backticks -- `span` -- and a
backtick cannot appear raw inside a JavaScript template literal, so plugin.js
escapes it as \\`. This unescapes that and *rejects any other backslash on the
JS side*; there are none in any raw-string body in the C++, so a second escape
could only be somebody hiding a difference.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half. `TraceLevels`, `SharpenCorners`,
`SimplifyLoop`, `Order`, `CutSpans`, `buildPath`, the flood schedule, Latch and
Live, and every conversion in plugin.js are a hand translation of Path.cpp,
Toolpath.cpp and Controls.cpp, and only a reader can tell whether they still
agree. When you change one of those, change it here too -- and remember that a
wrong mapping shows up on the page as a toolpath that is subtly the wrong
shape, which nobody will notice.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ file, C++ symbol, and whether the C++ is a raw string.
SHADERS = [
    ("VERSION", "source/Shaders.cpp", "kVersion", False),
    ("VERTEX_BODY", "source/Shaders.cpp", "kVertexBody", True),
    ("DETECT_BODY", "source/Shaders.cpp", "kDetectBody", True),
    ("BLUR_BODY", "source/Shaders.cpp", "kBlurBody", True),
    ("SEED_BODY", "source/Shaders.cpp", "kSeedBody", True),
    ("FLOOD_BODY", "source/Shaders.cpp", "kFloodBody", True),
    ("RESOLVE_BODY", "source/Shaders.cpp", "kResolveBody", True),
    ("SAMPLE_BODY", "source/Shaders.cpp", "kSampleBody", True),
    ("STAMP_VERTEX_BODY", "source/Shaders.cpp", "kStampVertexBody", True),
    ("STAMP_FRAGMENT_BODY", "source/Shaders.cpp", "kStampFragmentBody", True),
    ("COMPOSITE_BODY", "source/Shaders.cpp", "kCompositeBody", True),
]

# The C string escapes kVersion could plausibly use. Anything else is refused
# rather than guessed at.
C_ESCAPES = {"n": "\n", "t": "\t", "\\": "\\", '"': '"'}


def from_cpp(path, symbol, raw):
    with open(os.path.join(REPO, path)) as handle:
        source = handle.read()
    if raw:
        match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
        return None if match is None else match.group(1)

    match = re.search(r'const char\* const ' + symbol + r' = "((?:[^"\\]|\\.)*)";', source)
    if match is None:
        return None
    out = []
    text = match.group(1)
    i = 0
    while i < len(text):
        if text[i] == "\\":
            if text[i + 1] not in C_ESCAPES:
                return None
            out.append(C_ESCAPES[text[i + 1]])
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None

    body = match.group(1)

    # Undo the one escape the literal needs, and refuse the rest. The C++ raw
    # strings carry no backslash at all, so a stray one here is either a typo
    # or a difference being smuggled through the decoder. A `${` would be
    # interpolated by JavaScript, which is the same thing again.
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, at line {upto.count(chr(10)) + 1}"
    if "${" in body:
        upto = body[: body.index("${")]
        return None, f"template interpolation, at line {upto.count(chr(10)) + 1}"

    return body.replace("\\`", "`"), None


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()

    problems = 0
    for name, path, symbol, raw in SHADERS:
        cpp_text = from_cpp(path, symbol, raw)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {path}")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue

        if cpp_text == js_text:
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {path}")

        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break
        else:
            print("        the lines agree; the difference is a trailing newline")

    # Every piece the C++ assembles from must be carried. A body added to
    # Shaders.cpp and not to this list would otherwise go unchecked, and the
    # page would simply not have that pass.
    with open(os.path.join(REPO, "source", "Shaders.cpp")) as handle:
        declared = set(re.findall(r"const char\* const (k\w+) =", handle.read()))
    listed = {symbol for _, _, symbol, _ in SHADERS}
    for symbol in sorted(declared - listed):
        print(f"FAIL  {symbol} is in source/Shaders.cpp and this check does not compare it")
        problems += 1

    print()
    if problems:
        print(f"{problems} shader piece(s) differ -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shader pieces are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
