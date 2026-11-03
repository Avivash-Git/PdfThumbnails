# gate.manifest.<bin>: an executable of the app embeds exactly one application manifest
# (RT_MANIFEST resource #1, in one language: the linker merged its own into the template's, it
# did not add a second), and that manifest says what cmake/templates/<kind>.manifest.in says.
#   cmake -DFILE=<exe> -DKIND=settings|cli -DARCH=x64|arm64 -P CheckManifest.cmake
#
# Both kinds: one execution level, asInvoker without UI access; Windows 10/11 supportedOS;
# long paths; this architecture. settings: Per-Monitor V2 DPI awareness and Common Controls v6
# (the side-by-side binding to the copy in WinSxS; it does not stop an <exe>.local redirection,
# see cmake/Hardening.cmake). cli: neither (it shows no windows).

cmake_minimum_required(VERSION 4.3)
include("${CMAKE_CURRENT_LIST_DIR}/PeTools.cmake")
pdft_require(FILE KIND ARCH)
if(NOT KIND MATCHES "^(settings|cli)$")
  message(FATAL_ERROR "KIND must be settings or cli")
endif()

pdft_pe_resources(manifests "${FILE}" 24)
list(LENGTH manifests count)
if(NOT count EQUAL 1)
  pdft_gate_fail("${FILE} has ${count} manifest resources (${manifests}), expected exactly one")
endif()
string(REPLACE ":" ";" manifest "${manifests}")
list(GET manifest 0 id)
list(GET manifest 2 offset)
list(GET manifest 3 size)
if(NOT id STREQUAL "1")
  pdft_gate_fail("${FILE}: the manifest is resource ${id}, expected 1 (CREATEPROCESS_MANIFEST_RESOURCE_ID)")
endif()
file(READ "${FILE}" text OFFSET ${offset} LIMIT ${size})

set(problems "")
set(q "[\"']")   # the linker may write either quote

# pdft_count(<out-var> <regex>): how often <regex> occurs in the manifest.
function(pdft_count out regex)
  string(REGEX MATCHALL "${regex}" hits "${text}")
  list(LENGTH hits n)
  set(${out} ${n} PARENT_SCOPE)
endfunction()

# pdft_expect(<count> <regex> <what>): <regex> occurs exactly <count> times.
function(pdft_expect expected regex what)
  pdft_count(n "${regex}")
  if(NOT n EQUAL expected)
    set(problems ${problems} "${what}: found ${n} times, expected ${expected}" PARENT_SCOPE)
  endif()
endfunction()

if(ARCH STREQUAL "x64")
  set(manifest_arch "amd64")
else()
  set(manifest_arch "${ARCH}")
endif()

# Opening tags only; the linker's merge may add namespace prefixes (ms_compatibility:...) and
# attributes, and writes empty elements with closing tags.
set(open "<([A-Za-z_][A-Za-z0-9_]*:)?")
set(space "[ \t\r\n]")
pdft_expect(1 "${open}assembly[ \t\r\n>]" "the <assembly> root element")
pdft_expect(1 "${open}requestedExecutionLevel[ \t\r\n/>]" "requestedExecutionLevel")
pdft_expect(1 "${open}requestedExecutionLevel${space}[^>]*level=${q}asInvoker${q}"
            "requestedExecutionLevel level=\"asInvoker\"")
pdft_expect(1 "${open}requestedExecutionLevel${space}[^>]*uiAccess=${q}false${q}"
            "requestedExecutionLevel uiAccess=\"false\"")
pdft_expect(1 "${open}supportedOS${space}[^>]*Id=${q}{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}${q}"
            "supportedOS Windows 10/11")
pdft_expect(1 "${open}longPathAware[^>]*>true</" "longPathAware true")
pdft_expect(1 "${open}assemblyIdentity${space}[^>]*processorArchitecture=${q}${manifest_arch}${q}"
            "assemblyIdentity processorArchitecture=\"${manifest_arch}\"")
set(common_controls "${open}assemblyIdentity${space}[^>]*name=${q}Microsoft\\.Windows\\.Common-Controls${q}[^>]*>")
if(KIND STREQUAL "settings")
  pdft_expect(1 "${open}dpiAwareness[^>]*>PerMonitorV2</" "dpiAwareness PerMonitorV2")
  pdft_expect(1 "${common_controls}" "the Common Controls dependency")
  string(REGEX MATCH "${common_controls}" element "${text}")
  if(NOT element MATCHES "version=${q}6\\.0\\.0\\.0${q}")
    list(APPEND problems "the Common Controls dependency is not version 6.0.0.0: ${element}")
  endif()
else()
  pdft_expect(0 "${open}dpiAware" "DPI awareness")
  pdft_expect(0 "${common_controls}" "a Common Controls dependency")
endif()

if(problems)
  list(JOIN problems "\n  " problems)
  pdft_gate_fail("${FILE} manifest (resource 1, ${size} bytes):\n  ${problems}\n${text}")
endif()
if(KIND STREQUAL "settings")
  set(extra ", Per-Monitor V2, Common Controls v6")
else()
  set(extra ", no DPI awareness, no Common Controls")
endif()
pdft_gate_ok("${FILE}: one manifest (resource 1, ${size} bytes), asInvoker, Windows 10/11, long paths, "
             "${manifest_arch}${extra}")
