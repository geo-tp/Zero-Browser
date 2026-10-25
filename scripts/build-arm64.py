#!/usr/bin/env python3
"""Build with Debian 13 headers/runtimes from the official Cardputer BSP."""
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SDK = ROOT.parent / "Libretro_Zero/cp0-libretro-fb0/.cache/cp0/sdk-v0.0.4"

def output(*args):
    return subprocess.check_output(args, text=True).strip()

def main():
    if os.uname().machine == "aarch64":
        subprocess.run(["make", "-j2", "BUILD_DIR=build-arm64"], cwd=ROOT, check=True)
        return
    sdk = Path(os.environ.get("ZERO_BROWSER_SYSROOT", DEFAULT_SDK)).resolve()
    if not (sdk / "usr/include").is_dir():
        raise SystemExit("Set ZERO_BROWSER_SYSROOT to the official Debian 13 ARM64 BSP; see docs/BUILDING.md")
    cc, cxx = "aarch64-linux-gnu-gcc", "aarch64-linux-gnu-g++"
    runtime = sorted((sdk / "usr/lib/gcc/aarch64-linux-gnu").glob("*"), key=lambda p: tuple(map(int,p.name.split('.'))))[-1]
    libs = sdk / "usr/lib/aarch64-linux-gnu"
    cpp = [f"--sysroot={sdk}", "-nostdinc", "-idirafter", output(cc,"-print-file-name=include"), "-idirafter", str(sdk/"usr/include/aarch64-linux-gnu"), "-idirafter", str(sdk/"usr/include"), "-Isrc", "-Ivendor", "-Iassets", "-D_GNU_SOURCE"]
    flags = ["-O2", "-g", "-Wall", "-Wextra", "-Wpedantic", "-mcpu=cortex-a53", "-ffile-prefix-map="+str(ROOT)+"=."]
    cppflags = ["-std=c++17", "-nostdinc++", *flags]
    for p in [sdk/f"usr/include/c++/{runtime.name}", sdk/f"usr/include/aarch64-linux-gnu/c++/{runtime.name}", sdk/f"usr/include/c++/{runtime.name}/backward"]:
        cppflags += ["-isystem", str(p)]
    ld = [f"--sysroot={sdk}",f"-B{libs}/",f"-B{runtime}/",f"-L{runtime}",f"-L{libs}",f"-Wl,-rpath-link,{libs}"]
    env = os.environ.copy()
    for var in ["CPATH","C_INCLUDE_PATH","CPLUS_INCLUDE_PATH","LIBRARY_PATH"]:
        env.pop(var,None)
    gtk = ["-I"+str(sdk/"usr/include/glib-2.0"), "-I"+str(libs/"glib-2.0/include")]
    # The private Bookworm Qt SDK is separate from the Trixie supervisor BSP.
    # Never let pkg-config silently pick host Qt headers/libraries.
    qt_sdk = Path(os.environ.get("ZERO_BROWSER_QT_SYSROOT", ROOT / ".cache/qtwebkit-bookworm-arm64")).resolve()
    if os.environ.get("WITH_QTWEBKIT", "1") != "0" and not (qt_sdk / "usr/lib/aarch64-linux-gnu/pkgconfig/Qt5WebKitWidgets.pc").is_file():
        print("QtWebKit ARM64 headers missing; preparing isolated Bookworm SDK...")
        subprocess.run(["python3", str(ROOT / "scripts/prepare-qtwebkit-sysroot.py")], cwd=ROOT, check=True)
    qtlibs = qt_sdk / "usr/lib/aarch64-linux-gnu"
    qt = []
    if os.environ.get("WITH_QTWEBKIT", "1") != "0":
        # Do not ask pkg-config to prepend a sysroot: Bookworm Qt .pc files
        # can contain already rooted paths and produce duplicate prefixes.
        # More importantly, pkg-config returns a shell command line, not an
        # argv array; feeding it unescaped to Make breaks paths with spaces.
        # The isolated Qt SDK uses Debian's well-defined multiarch layout.
        qtinc = qt_sdk / "usr/include/aarch64-linux-gnu/qt5"
        modules = ["QtWebKitWidgets", "QtWebKit", "QtWidgets", "QtGui",
                   "QtNetwork", "QtCore", "QtPrintSupport"]
        required = [qtinc / "QtWidgets/QApplication", qtinc / "QtWebKitWidgets/QWebView",
                    qtlibs / "libQt5WebKitWidgets.so", qtlibs / "libQt5Widgets.so"]
        missing = [str(p) for p in required if not p.exists()]
        if missing:
            raise SystemExit("Incomplete QtWebKit Bookworm SDK; missing:\n  " +
                             "\n  ".join(missing) +
                             "\nRemove the incomplete .cache/qtwebkit-bookworm-arm64 directory "
                             "and rerun the build to download it again.")
        qtincludes = [qtinc, *(qtinc / module for module in modules)]
        qtflags = ["-I" + str(path) for path in qtincludes if path.is_dir()]
        qtflags += ["-DQT_WIDGETS_LIB", "-DQT_GUI_LIB", "-DQT_NETWORK_LIB", "-DQT_CORE_LIB"]
        qtlink = ["-L" + str(qtlibs), "-lQt5WebKitWidgets", "-lQt5WebKit",
                  "-lQt5Widgets", "-lQt5Gui", "-lQt5Network", "-lQt5Core"]
        qt = ["QT_CPPFLAGS=" + shlex.join(qtflags),
              "QT_LDLIBS=" + shlex.join(qtlink),
              "QT_LDFLAGS=" + shlex.join(["-L" + str(qtlibs),
                                            "-Wl,-rpath-link," + str(qtlibs)])]
    subprocess.run(["make","-j2","BUILD_DIR=build-arm64",f"CC={cc}",f"CXX={cxx}","CPPFLAGS="+shlex.join(cpp),"GTK_CPPFLAGS="+shlex.join(gtk),"CXXFLAGS="+shlex.join(cppflags),"CFLAGS="+shlex.join(["-std=c11",*flags]),"LDFLAGS="+shlex.join(ld),*qt],cwd=ROOT,env=env,check=True)
    binary = ROOT/"build-arm64/zero-browser"
    header=output("aarch64-linux-gnu-readelf","-h",str(binary))
    if "AArch64" not in header:
        raise SystemExit("Invalid binary architecture")
    print("Built ARM64 with",sdk)

if __name__ == "__main__":
    main()
