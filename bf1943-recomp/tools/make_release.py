"""Builds the player release: the launcher and the build trees it uses to make
the games from the player's own copies (launcher/setup.js). Nothing of the
games goes in: no game files, no recompiled code.

  python tools/make_release.py [out_dir]                 one exe (default)
  python tools/make_release.py [out_dir] --folder [--zip]  a folder
  python3 tools/make_release.py [out_dir] --appimage     Linux: one AppImage (run on Linux)
  python3 tools/make_release.py [out_dir] --mac          macOS: two dmgs, arm64 and x64 (run on a Mac)
  python  tools/make_release.py [out_dir] --server       the dedicated server package: BattlefieldServer-<version>.zip
                                                         and .tar.gz (sources only, any OS)

One exe (default out_dir build/release): BattlefieldLauncher-<version>.exe,
electron-builder's portable target (npm run dist in launcher/) with src/ and
src-bfbc1/ below inside it (staged in build/release_sources); it installs
in a BattlefieldRecomp folder next to itself (launcher/main.js).

A folder (default out_dir build/release/BF1943-PC):
  Launcher/        the packed launcher (npm run pack in launcher/)
  src/             this tree's sources (Battlefield 1943 and the code both games
                   share), the compiled shaders (build/xenia_shaders, so players
                   need no shader compiler), XenonRecomp.exe (tools/bin) and the
                   recompiler config (generated/*.toml and the switch tables)
  src-bfbc1/       ../bfbc1-recomp: Battlefield: Bad Company's sources and config
  README.txt
"""
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

# Left out of src/ (relative to the repo, '/' separated, prefixes).
SKIP = [
    'original/', 'generated/ppc/', 'generated/analysis/', 'build/', 'research/',
    'userdata',  # (every userdata* folder: saves and overrides made from the game's files)
    'frames/', 'shader_cache/', 'launcher/node_modules/', 'launcher/dist/',
    'server/arcadia360/bin/', 'server/arcadia360/obj/', 'tests/',
]
SKIP_NAMES = {'.git', '.git-upstream', '__pycache__', '.vs'}
SKIP_EXT = ('.log', '.out', '.wav', '.pdb', '.pipelinecache')
# Kept from the excluded folders.
KEEP = [
    'generated/bf1943_config.toml',
    'generated/analysis/bf1943_switch_tables_all.toml',
]
# Bad Company's tree: its sources and recompiler config only.
BC_ROOT = os.path.join(ROOT, '..', 'bfbc1-recomp')
BC_SKIP = ['original/', 'generated/', 'build/', 'userdata']  # (userdata, userdata2, userdata_perf, …)
BC_KEEP = [
    'generated/bfbc1_config.toml',
    'generated/analysis/bfbc1_switch_tables_all.toml',
]


def copy_tree(src_root, dst_root, skip=SKIP):
    for dirpath, dirnames, filenames in os.walk(src_root):
        rel_dir = os.path.relpath(dirpath, src_root).replace('\\', '/')
        rel_dir = '' if rel_dir == '.' else rel_dir + '/'
        dirnames[:] = [d for d in dirnames if d not in SKIP_NAMES and not any(
            (rel_dir + d + '/').startswith(s) for s in skip)]
        for f in filenames:
            rel = rel_dir + f
            if f.endswith(SKIP_EXT) or any(rel.startswith(s) for s in skip):
                continue
            os.makedirs(os.path.join(dst_root, rel_dir), exist_ok=True)
            shutil.copy2(os.path.join(dirpath, f), os.path.join(dst_root, rel))


def stage(out, src_name='src', bc_name='src-bfbc1', binaries=True):
    """The build trees: <out>/src and <out>/src-bfbc1 (the server package:
    bf1943-recomp and bfbc1-recomp, no shaders or XenonRecomp.exe: its
    build_server scripts make them)."""
    if os.path.exists(out):
        shutil.rmtree(out)
    src = os.path.join(out, src_name)
    print('sources ->', src)
    copy_tree(ROOT, src)
    # (The recompiler configs are not in git: a clean checkout, as in CI, has
    # none and build_linux.sh / build_server.bat make them from the player's
    # default.xex. The server package never carries them.)
    for k in KEEP if binaries else []:
        if os.path.isfile(os.path.join(ROOT, k)):
            os.makedirs(os.path.dirname(os.path.join(src, k)), exist_ok=True)
            shutil.copy2(os.path.join(ROOT, k), os.path.join(src, k))
    bc = os.path.join(out, bc_name)
    print('Bad Company ->', bc)
    copy_tree(BC_ROOT, bc, BC_SKIP)
    for k in BC_KEEP if binaries else []:
        if os.path.isfile(os.path.join(BC_ROOT, k)):
            os.makedirs(os.path.dirname(os.path.join(bc, k)), exist_ok=True)
            shutil.copy2(os.path.join(BC_ROOT, k), os.path.join(bc, k))
    if not binaries:
        return
    # (The Linux release: build_linux.sh builds the shaders and XenonRecomp
    # when they aren't there.)
    shaders = os.path.join(ROOT, 'build', 'xenia_shaders')
    if os.path.isdir(shaders):
        shutil.copytree(shaders, os.path.join(src, 'build', 'xenia_shaders'))
    elif os.name == 'nt':
        sys.exit('build/xenia_shaders missing: run tools/build_xenia_shaders.py first')
    recomp = os.path.join(ROOT, 'build', 'xenonrecomp-clang', 'XenonRecomp', 'XenonRecomp.exe')
    if not os.path.isfile(recomp):
        if os.name != 'nt':
            return
        sys.exit('XenonRecomp.exe missing: build thirdparty/XenonRecomp first (see the README)')
    os.makedirs(os.path.join(src, 'tools', 'bin'), exist_ok=True)
    shutil.copy2(recomp, os.path.join(src, 'tools', 'bin', 'XenonRecomp.exe'))


# ---- the dedicated server package ----------------------------------------------------
REPO = os.path.abspath(os.path.join(ROOT, '..'))
SERVER_FILES = {  # package path -> file (relative to the repo)
    'README-SERVER.txt': 'bf1943-recomp/tools/server_package/README-SERVER.txt',
    'build_server.sh': 'bf1943-recomp/tools/server_package/build_server.sh',
    'build_server.bat': 'bf1943-recomp/tools/server_package/build_server.bat',
    'docs/DEVELOPERS.md': 'docs/DEVELOPERS.md',
}


def launcher_version():
    with open(os.path.join(ROOT, 'launcher', 'package.json'), encoding='utf-8') as f:
        return json.load(f)['version']


def make_server(out):
    """BattlefieldServer-<version>.zip (Windows) and .tar.gz (Linux, macOS; the
    scripts executable): the sources the launcher builds from (no game files,
    no generated code, no binaries but AMD's FSR 3 DLLs the Windows build needs) plus the server scripts and docs."""
    name = 'BattlefieldServer-' + launcher_version()
    work = os.path.join(tempfile.gettempdir(), 'bf_server_package')  # (short: Windows path limit)
    pkg = os.path.join(work, name)
    stage(pkg, 'bf1943-recomp', 'bfbc1-recomp', binaries=False)
    for dst, rel in SERVER_FILES.items():
        os.makedirs(os.path.dirname(os.path.join(pkg, dst)), exist_ok=True)
        shutil.copy2(os.path.join(REPO, rel), os.path.join(pkg, dst))
    # Not needed in the trees: the package's own files, a third-party sample program.
    shutil.rmtree(os.path.join(pkg, 'bf1943-recomp', 'tools', 'server_package'), ignore_errors=True)
    shutil.rmtree(os.path.join(pkg, 'bf1943-recomp', 'thirdparty', 'VulkanMemoryAllocator', 'bin'), ignore_errors=True)
    os.makedirs(out, exist_ok=True)
    zip_path, tar_path = os.path.join(out, name + '.zip'), os.path.join(out, name + '.tar.gz')

    def entry(path):  # (bytes, is script): .sh LF, .bat CRLF whatever the checkout did
        with open(path, 'rb') as f:
            data = f.read()
        if path.endswith('.sh'):
            data = data.replace(b'\r\n', b'\n')
        elif path.endswith('.bat'):
            data = data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
        return data, path.endswith('.sh')

    files = []
    for dirpath, _, names in os.walk(pkg):
        for f in sorted(names):
            full = os.path.join(dirpath, f)
            files.append((os.path.relpath(full, work).replace('\\', '/'), full))
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z, \
            tarfile.open(tar_path, 'w:gz') as t:
        for arc, full in files:
            data, script = entry(full)
            zi = zipfile.ZipInfo(arc, (2020, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = ((0o755 if script else 0o644) | 0o100000) << 16
            z.writestr(zi, data)
            if arc.endswith(('.dll', '.lib')):
                continue  # (Windows only: AMD's FSR 3 libraries the Windows build copies; not for the Linux/macOS tarball)
            ti = tarfile.TarInfo(arc)
            ti.size, ti.mode = len(data), 0o755 if script else 0o644
            t.addfile(ti, io.BytesIO(data))
    for p in (zip_path, tar_path):
        print('done: %s (%.1f MB)' % (p, os.path.getsize(p) / 1e6))


def main():
    if '--server' in sys.argv:
        args = [a for a in sys.argv[1:] if not a.startswith('--')]
        make_server(os.path.abspath(args[0] if args else os.path.join(ROOT, 'build', 'release')))
        return
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    launcher = os.path.join(ROOT, 'launcher')
    if '--appimage' in sys.argv or '--mac' in sys.argv:
        # Linux (run there): BattlefieldLauncher-<version>-x86_64.AppImage, the
        # same build trees inside; it installs in a BattlefieldRecomp folder
        # next to itself and builds with build_linux.sh (launcher/setup_linux.js).
        # macOS (--mac, run there): BattlefieldLauncher-<version>-arm64.dmg and
        # -x64.dmg, unsigned; the same, built with build_macos.sh.
        mac = '--mac' in sys.argv
        out = os.path.abspath(args[0] if args else os.path.join(ROOT, 'build', 'release'))
        stage(os.path.join(ROOT, 'build', 'release_sources'))
        print('launcher -> ' + ('dmg' if mac else 'AppImage'))
        dist = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'bf_launcher_dist_' + ('mac' if mac else 'linux'))
        shutil.rmtree(dist, ignore_errors=True)
        subprocess.run(['npm', 'run', 'dist:mac' if mac else 'dist:linux', '--', '--config.directories.output=' + dist],
                       cwd=launcher, check=True)
        os.makedirs(out, exist_ok=True)
        for f in os.listdir(dist):
            if f.endswith('.dmg' if mac else '.AppImage'):
                shutil.copy2(os.path.join(dist, f), os.path.join(out, f))
                print('done:', os.path.join(out, f))
        return
    if '--folder' not in sys.argv:
        out = os.path.abspath(args[0] if args else os.path.join(ROOT, 'build', 'release'))
        stage(os.path.join(ROOT, 'build', 'release_sources'))  # launcher/package.json extraResources
        print('launcher -> one exe')
        # Built in the temp folder: something can hold a new .asar under the
        # source tree for a moment, and electron-builder fails on that (EBUSY).
        dist = os.path.join(os.environ.get('TEMP', ROOT), 'bf_launcher_dist')
        if os.path.exists(dist):
            shutil.rmtree(dist, ignore_errors=True)
        subprocess.run(f'npm run dist -- --config.directories.output="{dist}"', cwd=launcher, shell=True, check=True)
        os.makedirs(out, exist_ok=True)
        for f in os.listdir(dist):
            if f.startswith('BattlefieldLauncher-') and f.endswith('.exe'):
                shutil.copy2(os.path.join(dist, f), os.path.join(out, f))
                print('done:', os.path.join(out, f))
        return

    out = os.path.abspath(args[0] if args else os.path.join(ROOT, 'build', 'release', 'BF1943-PC'))
    stage(out)
    print('launcher -> Launcher/')
    subprocess.run('npm run pack', cwd=launcher, shell=True, check=True)
    shutil.copytree(os.path.join(launcher, 'dist', 'BF1943Launcher-win32-x64'), os.path.join(out, 'Launcher'))
    with open(os.path.join(out, 'README.txt'), 'w', encoding='utf-8') as f:
        f.write('Battlefield 1943 and Battlefield: Bad Company for PC\n\n'
                'Start Launcher\\BF1943Launcher.exe and pick a game at the top. The first time, it\n'
                'builds that game from your own copy:\n'
                '  Battlefield 1943: the Xbox 360 package (version 1.06) or default.xex.\n'
                '  Battlefield: Bad Company: the disc image (.iso, USA) or default.xex.\n'
                'Choose the file, press INSTALL and keep the launcher open. It installs the build tools\n'
                'it needs (LLVM, CMake, Visual Studio C++ build tools, Vulkan SDK) through winget.\n'
                'Each game takes about 20-60 minutes to build.\n')
    if '--zip' in sys.argv:
        shutil.make_archive(out, 'zip', os.path.dirname(out), os.path.basename(out))
        print('zip ->', out + '.zip')
    print('done:', out)


if __name__ == '__main__':
    main()
