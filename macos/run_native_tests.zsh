#!/bin/zsh
# macos/run_native_tests.zsh — build and run the native tests on the Mac.
#
# Links each standalone *_tests.cpp and the Tests/LlamaBossTests runner
# against the dev build's app objects (everything but app.cpp, as a static
# library, so each test pulls in only what it uses).  Run after
# `cmake --build build_macos`.  Output: build_macos/native_tests/.
#
# Known on macOS: command_policy_tests checks the PowerShell policy, which the
# Mac build replaces with the zsh policy (command_policy_zsh_tests), so its
# failures are expected; the Tests/ var_store cases expect "Vars\" separators.
set -u
R=${0:A:h:h}; B=$R/build_macos; O=$B/native_tests
mkdir -p $O/bin $O/logs $O/runner
rm -f $O/libapp.a
ar rcs $O/libapp.a ${(@)$(ls $B/CMakeFiles/LlamaBoss.dir/*.o | grep -v '/app.cpp.o$')}
CXX=(c++ -std=c++17 -g -arch arm64 -DPOCO_UNBUNDLED -D_FILE_OFFSET_BITS=64 -DwxDEBUG_LEVEL=0
     -I$R -I$R/src/platform -I$R/Tests/LlamaBossTests -isystem /opt/homebrew/include
     -isystem /opt/homebrew/opt/openssl@3/include $(wx-config-3.2 --cxxflags))
LIBS=($O/libapp.a $B/libllama_platform.a -L/opt/homebrew/lib $(wx-config-3.2 --libs all)
      -lPocoNetSSL -lPocoCrypto -lPocoUtil -lPocoXML -lPocoZip -lPocoNet -lPocoJSON -lPocoFoundation
      -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto -liconv
      -framework Cocoa -framework Security -framework CoreFoundation -Wl,-rpath,/opt/homebrew/lib)
build() { local n=$1; shift; "${CXX[@]}" -o $O/bin/$n "$@" "${LIBS[@]}" > $O/logs/$n.build.txt 2>&1; }

for t in $R/*_tests.cpp; do
    n=${t:t:r}
    if ! build $n $t; then echo "BUILD FAIL $n"; continue; fi
    (cd $R && $O/bin/$n > $O/logs/$n.txt 2>&1 < /dev/null); rc=$?
    printf '%-26s rc=%d  %s\n' $n $rc "$(tail -1 $O/logs/$n.txt)"
done
if build LlamaBossTests $R/Tests/LlamaBossTests/*.cpp; then
    (cd $O/runner && $O/bin/LlamaBossTests --cases $R/Tests/LlamaBossTests/Cases \
        > $O/logs/LlamaBossTests.txt 2>&1 < /dev/null)
    printf '%-26s %s\n' LlamaBossTests "$(grep -E 'passed, [0-9]+ failed' $O/logs/LlamaBossTests.txt)"
    grep '^\[FAIL\]' $O/logs/LlamaBossTests.txt
else
    echo "BUILD FAIL LlamaBossTests"
fi
