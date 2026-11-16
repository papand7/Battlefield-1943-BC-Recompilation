"""Fetches what AMD FSR 3.1 frame generation needs (the FidelityFX SDK 2.4.0 FFX
API: the Vulkan frame generation swapchain + frame generation context) into
thirdparty/fidelityfx_fg: the headers and amd_fidelityfx_framegeneration_vk.dll
(AMD's signed build, loaded next to the exe by the FFX loader that the FSR 3
upscaler already ships: thirdparty/fidelityfx/bin), each checked against its
SHA-256. Nothing is run from what is downloaded. The headers are MIT; the DLL
may be redistributed as it is, in binary form (thirdparty/fidelityfx_fg/license.md
once fetched). Not kept in this repo (36 MB): the Windows build runs this by
itself when it is missing; without it the frame generation option is off.

  python -I tools/fetch_fsr3fg.py [--to <dir>]
"""
import hashlib
import os
import sys
import urllib.request

TAG = 'v2.4.0'
URL = 'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/' + TAG + '/Kits/FidelityFX/'
# (path in AMD's repo under Kits/FidelityFX/, path here, SHA-256)
FILES = [
    ('api/include/ffx_api.h', 'api/include/ffx_api.h',
     '4d9dcf243ef5efbf03340a02f16b51a22053243a22e8a9a0b3ce91c26982b4bf'),
    ('api/include/ffx_api.hpp', 'api/include/ffx_api.hpp',
     'af441486342abfeb189c5672f3216a28446b582ad314ec4e754b81e20bb8a840'),
    ('api/include/ffx_api_loader.h', 'api/include/ffx_api_loader.h',
     '18d1901dbbc881e8131b91c65ad44bb3b4a1de6c31435d5b75236f16727ba80e'),
    ('api/include/ffx_api_types.h', 'api/include/ffx_api_types.h',
     '53f07c113a9ba493f297656e4481b6539aed0afc9f3cc8da682aa477d270c46c'),
    ('api/include/vk/ffx_api_vk.h', 'api/include/vk/ffx_api_vk.h',
     '09ac1855bc4c2f711243d6982909385072ca3f8d30db44d1a82ee5bd637832ad'),
    ('api/include/vk/ffx_api_vk.hpp', 'api/include/vk/ffx_api_vk.hpp',
     '7757c3ffa6b1e189bca5eec3b8c93aa8f802386642648a0dfeb8c2c52502e5ba'),
    ('framegeneration/include/ffx_framegeneration.h', 'framegeneration/include/ffx_framegeneration.h',
     'bc78a18aa95f5157b4699b0602700592b624788c5bc55d1762cb46a1b6557ca1'),
    ('framegeneration/include/ffx_framegeneration_api_types.h',
     'framegeneration/include/ffx_framegeneration_api_types.h',
     '396596af3df63310674e8036abd2ad0fcd061e61334e7a2c76653a2be3c686b4'),
    ('framegeneration/include/vk/ffx_api_framegeneration_vk.h',
     'framegeneration/include/vk/ffx_api_framegeneration_vk.h',
     '321d3138005757759d9ee05281d3dcd4cc2c3608bc1d524f9b1d0a6dadd49f61'),
    ('signedbin/amd_fidelityfx_framegeneration_vk.dll', 'bin/amd_fidelityfx_framegeneration_vk.dll',
     'd002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4'),
    ('docs/license.md', 'license.md',
     '32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb'),
]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    to = os.path.abspath(sys.argv[sys.argv.index('--to') + 1] if '--to' in sys.argv else
                         os.path.join(here, '..', 'thirdparty', 'fidelityfx_fg'))
    for remote, local, sha in FILES:
        dest = os.path.join(to, local)
        if os.path.isfile(dest) and hashlib.sha256(open(dest, 'rb').read()).hexdigest() == sha:
            continue
        print('fetching', remote, flush=True)
        req = urllib.request.Request(URL + remote, headers={'User-Agent': 'bf-recomp-fetch-fsr3fg'})
        with urllib.request.urlopen(req, timeout=300) as r:
            data = r.read()
        got = hashlib.sha256(data).hexdigest()
        if got != sha:
            sys.exit('error: %s is not the expected file (SHA-256 %s)' % (remote, got))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest + '.part', 'wb') as f:
            f.write(data)
        os.replace(dest + '.part', dest)
    print('FSR 3 frame generation SDK ready in', to)


if __name__ == '__main__':
    main()
