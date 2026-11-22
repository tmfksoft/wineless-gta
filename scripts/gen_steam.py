#!/usr/bin/env python3
"""Generate src/steam/steam_ifaces.inc from Steamworks SDK headers.

usage: gen_steam.py <dir with steamworks_sdk_NNN folders> > src/steam/steam_ifaces.inc

The headers are only read at development time (Proton's lsteamclient keeps
every historical SDK: lsteamclient/steamworks_sdk_<ver>); the generated table
holds method names, vtable positions and calling signatures.

Signature string: return code, then one code per argument.
  return: v void, i 32-bit, b bool, q 64-bit (edx:eax), S CSteamID/CGameID
          (MSVC returns these through a hidden pointer), f float, z const char*
          (copied into guest memory), I interface pointer (wrapped), x unsupported
  args:   i 32-bit or smaller, p pointer, q 64-bit, f float, d double,
          x unsupported (the call is refused and logged)
"""
import re
import sys

# (SDK, class, version string, header). Versions = what games and Steam's DRM ask
# for: SDK 1.35 (Age of Empires II HD's steam_api) and SDK 1.04 (SteamClient008,
# used by the Steam DRM wrapper of older games such as GTA Vice City).
# The first entry for a class is the one host-side code calls.
IFACES = [
    ("135", "ISteamClient", "SteamClient017", "isteamclient.h"),
    ("135", "ISteamUser", "SteamUser018", "isteamuser.h"),
    ("135", "ISteamFriends", "SteamFriends015", "isteamfriends.h"),
    ("135", "ISteamUtils", "SteamUtils007", "isteamutils.h"),
    ("135", "ISteamMatchmaking", "SteamMatchMaking009", "isteammatchmaking.h"),
    ("135", "ISteamUserStats", "STEAMUSERSTATS_INTERFACE_VERSION011", "isteamuserstats.h"),
    ("135", "ISteamApps", "STEAMAPPS_INTERFACE_VERSION007", "isteamapps.h"),
    ("135", "ISteamNetworking", "SteamNetworking005", "isteamnetworking.h"),
    ("135", "ISteamRemoteStorage", "STEAMREMOTESTORAGE_INTERFACE_VERSION013", "isteamremotestorage.h"),
    ("135", "ISteamUGC", "STEAMUGC_INTERFACE_VERSION007", "isteamugc.h"),
    ("135", "ISteamScreenshots", "STEAMSCREENSHOTS_INTERFACE_VERSION002", "isteamscreenshots.h"),
    ("135", "ISteamHTTP", "STEAMHTTP_INTERFACE_VERSION002", "isteamhttp.h"),
    ("104", "ISteamClient", "SteamClient008", "isteamclient.h"),
    ("104", "ISteamUser", "SteamUser012", "isteamuser.h"),
    ("104", "ISteamUtils", "SteamUtils004", "isteamutils.h"),
    ("104", "ISteamApps", "STEAMAPPS_INTERFACE_VERSION003", "isteamapps.h"),
    ("104", "ISteamFriends", "SteamFriends005", "isteamfriends.h"),
    ("104", "ISteamUserStats", "STEAMUSERSTATS_INTERFACE_VERSION005", "isteamuserstats.h"),
    ("104", "ISteamRemoteStorage", "STEAMREMOTESTORAGE_INTERFACE_VERSION002", "isteamremotestorage.h"),
    ("104", "ISteamNetworking", "SteamNetworking002", "isteamnetworking.h"),
    ("104", "ISteamMatchmaking", "SteamMatchMaking006", "isteammatchmaking.h"),
]

U64 = {"uint64", "int64", "CSteamID", "CGameID", "SteamAPICall_t", "PublishedFileId_t", "UGCHandle_t",
       "UGCFileWriteStreamHandle_t", "UGCQueryHandle_t", "UGCUpdateHandle_t", "SteamLeaderboard_t",
       "SteamLeaderboardEntries_t", "PublishedFileUpdateHandle_t", "uint64_steamid", "ManifestId_t",
       "SteamInventoryResult_t", "SteamItemInstanceID_t"}
# function pointers / structs the guest would hand to Steam: refuse rather than crash
UNSAFE = {"SteamAPIWarningMessageHook_t", "SteamAPI_PostAPIResultInProcess_t", "SteamAPI_CheckCallbackRegistered_t",
          "ISteamMatchmakingServerListResponse", "ISteamMatchmakingPingResponse",
          "ISteamMatchmakingPlayersResponse", "ISteamMatchmakingRulesResponse", "MatchMakingKeyValuePair_t",
          "SteamParamStringArray_t"}


def strip(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    out, skip = [], []
    for line in src.split("\n"):
        s = line.strip()
        if s.startswith("#if"):
            skip.append("_PS3" in s or "0" == s.split()[-1])
            continue
        if s.startswith("#else") and skip:
            skip[-1] = not skip[-1]
            continue
        if s.startswith("#endif") and skip:
            skip.pop()
            continue
        if s.startswith("#") or any(skip):
            continue
        out.append(line)
    return "\n".join(out)


def base_type(t):
    t = re.sub(r"\b(const|struct|class|enum|unsigned|signed)\b", " ", t)
    return t.replace("*", " ").replace("&", " ").split()[-1] if t.split() else t


def arg_code(t):
    t = t.strip()
    b = base_type(t)
    if b in UNSAFE:
        return "x"
    if "*" in t or "&" in t:
        return "p"
    if b in U64:
        return "q"
    if b == "float":
        return "f"
    if b == "double":
        return "d"
    return "i"


def ret_code(t, name):
    t = t.strip()
    b = base_type(t)
    if t == "void":
        return "v"
    if t.startswith("ISteam") or re.match(r"(const\s+)?ISteam\w+\s*\*", t):
        return "I"
    if "*" in t:
        return "z" if b == "char" else "x"
    if b in ("CSteamID", "CGameID"):
        return "S"
    if b in U64:
        return "q"
    if b == "bool":
        return "b"
    if b == "float":
        return "f"
    if b == "double":
        return "x"
    return "i"


def split_args(a):
    a = a.strip()
    if not a or a == "void":
        return []
    parts, depth, cur = [], 0, ""
    for ch in a:
        if ch in "(<":
            depth += 1
        elif ch in ")>":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    parts.append(cur)
    res = []
    for p in parts:
        p = p.split("=")[0].strip()          # default values
        p = re.sub(r"\b[A-Z_]+\(\s*[^)]*\)", "", p)  # annotation macros
        p = re.sub(r"\[\s*\w*\s*\]", "*", p)  # arrays decay to pointers
        m = re.match(r"(.*?)(\w+)$", p.strip(), re.S)
        res.append(m.group(1).strip() if m and m.group(1).strip() else p.strip())
    return res


def methods(path, cls):
    src = strip(open(path, encoding="latin1").read())
    m = re.search(r"class\s+%s\s*(:[^{]*)?\{(.*?)\n\};" % cls, src, re.S)
    if not m:
        sys.exit("class %s not found in %s" % (cls, path))
    body = m.group(2)
    res = []
    for mm in re.finditer(r"virtual\s+([^;]*?)\b(\w+)\s*\(([^;]*?)\)\s*(const\s*)?=\s*0\s*;", body, re.S):
        rtype = re.sub(r"\b[A-Z_]+\(\s*[^)]*\)", "", mm.group(1)).strip()
        res.append((mm.group(2), rtype, split_args(mm.group(3))))
    return res


def msvc_order(ms):
    """MSVC groups overloads at the first declaration, in reverse order."""
    order, seen = [], set()
    for i, (name, _, _) in enumerate(ms):
        if name in seen:
            continue
        seen.add(name)
        group = [j for j, x in enumerate(ms) if x[0] == name]
        order.extend(reversed(group))
    return order


def main():
    d = sys.argv[1]
    print("// Generated by scripts/gen_steam.py from Steamworks SDK headers. Do not edit.")
    print("// { method name, Linux (Itanium) vtable index, signature } in Windows (MSVC) vtable order.")
    tname = lambda cls, ver: "m_%s_%s" % (cls, re.sub(r"\W", "_", ver))
    for sdk, cls, ver, hdr in IFACES:
        ms = methods("%s/steamworks_sdk_%s/%s" % (d, sdk, hdr), cls)
        print("static const SteamMethod %s[] = {" % tname(cls, ver))
        for host_idx in msvc_order(ms):
            name, rtype, args = ms[host_idx]
            sig = ret_code(rtype, name) + "".join(arg_code(a) for a in args)
            print('    {"%s", %d, "%s"},' % (name, host_idx, sig))
        print("};")
    print("static const SteamIfaceDesc steam_ifaces[] = {")
    for sdk, cls, ver, hdr in IFACES:
        t = tname(cls, ver)
        print('    {"%s", "%s", %s, sizeof %s / sizeof *%s},' % (cls, ver, t, t, t))
    print("};")


main()
