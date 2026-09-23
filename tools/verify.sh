#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go, in the order that
# fails fastest.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   build         a FRESH universal Release build. Not the dev build: CMake
#                 latches the architecture list at the first target, so the
#                 only build worth measuring is one configured from nothing.
#   shaders       does every shader compile, through a real GLSL compiler
#                 (tools/check-shaders.sh, which CI runs too).
#   offline       the checks that need no GL -- what CI runs on a runner with
#                 no GPU:
#                   --names     nothing the host will silently truncate
#                   --exact     the reference EDT is brute force's, exactly
#                   --march     the tracer on analytic fields: circles to
#                               cell^2/(8 rho), corners to the ULP, the job
#                               ordered one pocket at a time
#                   --negative-offline   and each of those can fail
#   geometry      every GL check, at TWO rasters: 320x180, which is what CI
#                 renders at, and 1280x720. A check that holds at one raster
#                 was fitted to it. Each is measured out of the picture or the
#                 plugin's own field:
#                   --distance  the flood is the exact EDT of its working
#                               lattice (two pixels a texel): to 6 ULP on
#                               rectilinear shapes, sqrt2 texels on curved
#                               ones and on the constellation plain JFA
#                               misses
#                   --fillet    inside corners keep a fillet of radius r
#                   --slot      under 2r never entered; over 2r cut through
#                   --scallop   ridges of s - 2r past s = 2r; none below
#                   --feed      Feed px of path a second, at 60 and 30 fps
#                   --latch     Restart clears the part; a resize keeps it
#                   --lattice   the field IS on the working lattice: its
#                               size, and its sign at every pixel of a
#                               fixture a full-raster field cannot match
#                   --negative  every one of those FAILS on a perturbed plugin
#   pipe          the fleet's --pipe contract: whole frames only, an unknown
#                 cue refused, a closed stdout a failure.
#   sweep         does every control change the picture. A GLSL uniform whose
#                 name does not match the C++ is ignored without a word.
#   bench         the render cost at 720p and 1080p, for the record. Not
#                 pass/fail. 4K is `tptest --bench-4k`, once, by hand: this
#                 Mac is shared.
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the build really universal.
#   plist         does CFBundleExecutable name the binary that is on disk.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         a real FFGL host loads the bundle and reports the name, id
#                 and type it sees -- the name field is not null-terminated
#                 and a host truncates silently past 16 characters.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

step "build (fresh universal Release, $BUILD)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

TPTEST="$BUILD/tptest"

step "shaders"
if out=$(tools/check-shaders.sh "$TPTEST" 2>&1); then
	pass "$( printf '%s\n' "$out" | tail -1 | sed 's/^ *//' )"
else
	fail "tools/check-shaders.sh"
	printf '%s\n' "$out" | sed 's/^/      /'
fi

step "offline (no GL)"
for check in names exact march negative-offline; do
	if out=$("$TPTEST" --$check 2>&1); then
		pass "tptest --$check: $( printf '%s\n' "$out" | grep -v '^$' | grep -v -e '^offline:' -e '^and the GL' | tail -1 )"
	else
		fail "tptest --$check"
		printf '%s\n' "$out" | sed 's/^/      /'
	fi
done

for size in 320x180 1280x720; do
	step "geometry at $size"
	for check in distance fillet slot scallop feed latch lattice negative; do
		if out=$("$TPTEST" --$check --size $size 2>&1); then
			pass "tptest --$check: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
		else
			fail "tptest --$check at $size"
			printf '%s\n' "$out" | sed 's/^/      /'
		fi
	done
done

#---------------------------------------------------------------------------
# --pipe, in the fleet's frame format. Two and a half frames in must be exactly
# two frames out and a clean exit -- a partial frame is the end of the stream,
# never a frame -- a cue naming no parameter must be refused rather than
# silently doing nothing to a take, and a reader that goes away must be a
# failure, not a render into nothing.
#---------------------------------------------------------------------------
step "pipe"
frame=$(( 64 * 36 * 4 ))
raw=$( mktemp ); cues=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
got=$( "$TPTEST" --pipe --size 64x36 < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi
# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever tptest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
"$TPTEST" --pipe --size 64x36 --script "$cues" < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then
	pass "a cue naming no parameter is refused (exit 2)"
else
	fail "a cue naming no parameter gave exit $status, not 2"
fi
# A reader that has gone: stdout closed before the first frame is written,
# and a reader that takes ten bytes and leaves (SIGPIPE, unless ignored,
# would make that exit 141 with no word on stderr).
head -c $(( frame * 40 )) /dev/zero > "$raw"
"$TPTEST" --pipe --size 64x36 < "$raw" >&- 2>/dev/null
status=$?
if [ "$status" -eq 1 ]; then
	pass "a closed stdout ends the render with exit 1"
else
	fail "a closed stdout gave exit $status, not 1"
fi
"$TPTEST" --pipe --size 64x36 < "$raw" 2>/dev/null | head -c 10 >/dev/null
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ]; then
	pass "a reader that leaves mid-stream ends the render with exit 1"
else
	fail "a reader that left mid-stream gave exit $status, not 1"
fi
rm -f "$raw" "$cues"

step "sweep"
if out=$(python3 tools/sweep.py --binary "$TPTEST" 2>/dev/null); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "tools/sweep.py reports a dead control"
	printf '%s\n' "$out" | grep -E '^DEAD|DEAD CONTROLS' | sed 's/^/      /'
fi

step "bench (for the record)"
"$TPTEST" --bench --frames 60 2>&1 | sed -n '3,6p' | sed 's/^/   /'

BUNDLE="$BUILD/Toolpath.bundle"
BIN="$BUNDLE/Contents/MacOS/Toolpath"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	if [ "$ident" = "com.stoatworks.ffgl.toolpath" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Toolpath.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		for want in "name:        SW Toolpath" "id:          TP01" "type:        effect"; do
			case "$probe" in
				*"$want"*) pass "host sees '$want'" ;;
				*) fail "host does not see '$want' -- see: $OXBOW probe $BUNDLE" ;;
			esac
		done
		self=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$self" in
			*"selftest:    PASS"*) pass "instantiates through plugMain and renders 120 frames" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
