"""Summarise a match replay (.bfrp, runtime/replay.h).

usage: replay_dump.py <file.bfrp> [--frames N]   (N: print every Nth frame)
"""
import struct
import sys


def main():
    path = sys.argv[1]
    every = int(sys.argv[sys.argv.index('--frames') + 1]) if '--frames' in sys.argv else 0
    d = open(path, 'rb').read()
    assert d[:4] == b'BFRP', 'not a replay'
    version, tick, start = struct.unpack_from('<IIq', d, 4)
    mp = d[20:52].split(b'\0')[0].decode()
    at = 52
    players, vehicles, frames, kills, points, end = {}, {}, 0, [], [], None
    last_t = 0
    try:
        frames, last_t, end = read(d, at, players, vehicles, kills, points, every)
    except struct.error:
        frames, last_t, end = read.frames, read.last_t, None  # (still being written: cut short)
    print(f'{path}: version {version}, map {mp}, tick {tick} ms, {frames} frames, {last_t / 1000:.0f} s'
          f'{"" if end is not None else " (not finished)"}, {len(d)} bytes')
    for pid, (name, team, flags) in sorted(players.items()):
        print(f'  player {pid}: {name} team {team}{" bot" if flags & 1 else ""}')
    print(f'  vehicles: {len(vehicles)}: {", ".join(sorted(set(vehicles.values())))}')
    for k in kills:
        vic, kil = players.get(k[1], ('?',))[0], players.get(k[2], ('?',))[0] if k[2] != 0xFFFF else '?'
        print(f'  kill {k[0] / 1000:6.1f}s: {kil} killed {vic} at {k[3]:.0f} {k[4]:.0f} {k[5]:.0f}')
    for p in points:
        print(f'  flag {p[1]} -> team {p[2]} at {p[0] / 1000:.1f}s ({p[3]:.0f} {p[5]:.0f})')


def read(d, at, players, vehicles, kills, points, every):
    read.frames, read.last_t = 0, 0
    frames, last_t, end = 0, 0, None
    while at < len(d):
        t = d[at]
        at += 1
        if t == 1:
            pid, team, flags = struct.unpack_from('<HBB', d, at)
            name = d[at + 4:at + 20].split(b'\0')[0].decode()
            players[pid] = (name, team, flags)
            at += 20
        elif t == 2:
            ft, n = struct.unpack_from('<IH', d, at)
            at += 6
            sol = [struct.unpack_from('<HBB5f', d, at + i * 24) for i in range(n)]
            at += n * 24
            (nv,) = struct.unpack_from('<H', d, at)
            at += 2
            veh = [struct.unpack_from('<H5f', d, at + i * 22) for i in range(nv)]
            at += nv * 22
            if every and frames % every == 0:
                print(f'{ft / 1000:7.1f}s  {n} soldiers  {nv} vehicles')
                for s in sol:
                    print(f'    {players.get(s[0], ("?",))[0]:10s} {"V" if s[1] & 1 else " "} hp {s[2] * 100 / 255:3.0f}'
                          f'  {s[3]:8.1f} {s[4]:6.1f} {s[5]:8.1f}  yaw {s[6]:5.2f} pitch {s[7]:5.2f}')
            frames += 1
            last_t = ft
            read.frames, read.last_t = frames, last_t
        elif t == 3:
            (vid,) = struct.unpack_from('<H', d, at)
            vehicles[vid] = d[at + 2:at + 26].split(b'\0')[0].decode()
            at += 26
        elif t == 4:
            kills.append(struct.unpack_from('<IHH3f', d, at))
            at += 20
        elif t == 5:
            points.append(struct.unpack_from('<IHB3f', d, at))
            at += 19
        elif t == 6:
            (end,) = struct.unpack_from('<I', d, at)
            at += 4
        else:
            print(f'unknown record {t} at {at - 1}')
            break
    return frames, last_t, end


if __name__ == '__main__':
    main()
