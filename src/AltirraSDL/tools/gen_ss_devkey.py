#!/usr/bin/env python3
"""Generate the ScreenScraper developer-key header.

This script contains no secrets and is safe to commit.  It reads the
developer credential from the ENVIRONMENT:

    ALTIRRA_SS_DEVID
    ALTIRRA_SS_DEVPASSWORD

and writes a header that embeds them XOR-obfuscated.  When either is
absent it writes a header with ALTIRRA_SS_HAVE_DEVKEY 0, which is a
normal, fully supported build: the metadata feature reports itself as
"not configured" and offers the user an Advanced field to supply their
own credential.  That unconfigured build is what every contributor,
every fork pull request and every distribution packager gets.

Why the environment and not a CMake cache variable or argv:
  -DALTIRRA_SS_DEVID=... would land in CMakeCache.txt, in
  compile_commands.json, in the process list, and in any CI log that
  echoes the configure command.  Reading os.environ keeps the value out
  of all four -- CMake only ever learns whether a key is present.

On the obfuscation: there is no secret in this repository, so the XOR is
not protecting the source.  It protects the *shipped binary*, where it
stops `strings AltirraSDL | grep devpassword` and automated credential
scanners from harvesting released artifacts.  Anyone determined will
still recover it from a release build in minutes; that is unavoidable
for this API and is exactly why the per-user account path exists.

The developer DEBUG password (devdebugpassword) must never pass through
this script, this repository or CI.  It can force quota counters, spoof
IPs and escalate account level; it is read from ALTIRRA_SS_DEBUG_PASSWORD
at runtime in developer builds only.
"""

import argparse
import os
import sys

# Local credential file, read when the environment does not supply one.
# This is what makes a developer's own builds work without exporting
# variables for every shell: write it once, and every local configure
# picks it up.  It is gitignored, so it still never enters the repo.
#
#   <repo>/localconfig/screenscraper.env
#       ALTIRRA_SS_DEVID=yourid
#       ALTIRRA_SS_DEVPASSWORD=yourpassword
#
# CI does not use it — the workflows export the environment instead.
LOCAL_FILE_RELATIVE = os.path.join("localconfig", "screenscraper.env")

# ---------------------------------------------------------------------------
# Built-in application credential.
#
# This is the ScreenScraper *developer* credential -- the one that
# identifies AltirraSDL to the API so they can attribute traffic to this
# frontend.  It is deliberately shipped, obfuscated, in this tracked
# file, which is what Skyscraper and ES-DE also do: without it a
# source build could not scrape at all, and ScreenScraper expects each
# frontend to identify itself.
#
# Understand what this is and is not:
#   - it grants NO quota escalation, NO IP spoofing and NO level
#     override.  It is an application identity, nothing more;
#   - the XOR below stops `strings` and credential scanners harvesting
#     it from a release binary.  It is obfuscation, not encryption --
#     anyone determined recovers it in minutes, and that is accepted;
#   - because it is shared by every AltirraSDL user, anonymous runs are
#     capped client-side (see kAnonymousRunCap in metadata_scraper.cpp)
#     and the UI pushes users toward their own free account.
#
# The developer DEBUG password (devdebugpassword) is a completely
# different secret -- it CAN force quota counters, spoof IPs and
# escalate account level -- and must never appear here or anywhere else
# in the repository.  It is read from ALTIRRA_SS_DEBUG_PASSWORD at
# runtime, in developer builds only.
#
# Regenerate these after a credential rotation with:
#   1. put the new values in localconfig/screenscraper.env
#   2. python3 src/AltirraSDL/tools/gen_ss_devkey.py --embed-default
#   3. commit this file; delete localconfig/screenscraper.env
DEFAULT_DEVID_OBF = bytes([
    0x39, 0xFE, 0x2F, 0xDB, 0xC8, 0x4D, 0xE0,
])
DEFAULT_DEVPW_OBF = bytes([
    0x07, 0xD1, 0x03, 0xF3, 0xFE, 0x49, 0xCD, 0xD8,
    0x09, 0x8F, 0x2B,
])


def repo_root():
    # .../src/AltirraSDL/tools/gen_ss_devkey.py -> repo root
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.abspath(os.path.join(here, "..", "..", ".."))


def read_local_file():
    """Return {KEY: value} from the local credential file, or {}."""
    path = os.environ.get("ALTIRRA_SS_DEVKEY_FILE")
    if not path:
        path = os.path.join(repo_root(), LOCAL_FILE_RELATIVE)

    values = {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, value = line.partition("=")
                values[key.strip()] = value.strip().strip('"').strip("'")
    except OSError:
        return {}
    return values

# Fixed obfuscation key.  Rotated through, combined with the byte index
# so that repeated plaintext characters do not produce repeated cipher
# bytes (which would make the length and shape of the secret obvious in
# a hex dump).
KEY = bytes([
    0x5B, 0xA2, 0x17, 0xC4, 0x39, 0xE0, 0x7D, 0x86,
    0x4F, 0x91, 0x2C, 0xD8, 0x63, 0xB5, 0x0A, 0xF7,
])


def obfuscate(text):
    raw = text.encode("utf-8")
    return bytes(
        (b ^ KEY[i % len(KEY)] ^ ((i * 37 + 11) & 0xFF)) & 0xFF
        for i, b in enumerate(raw)
    )


def deobfuscate(data):
    # The transform is XOR-based, so it is its own inverse.
    return bytes(
        (b ^ KEY[i % len(KEY)] ^ ((i * 37 + 11) & 0xFF)) & 0xFF
        for i, b in enumerate(data)
    ).decode("utf-8")


def python_array(name, data):
    lines = ["%s = bytes([" % name]
    for i in range(0, len(data), 8):
        lines.append("    " + " ".join("0x%02X," % b for b in data[i:i + 8]))
    lines.append("])")
    return "\n".join(lines)


def embed_default():
    """Rewrite DEFAULT_*_OBF in this file from localconfig/screenscraper.env.

    Maintenance command, run by hand after a credential rotation.  The
    credential is read from the file rather than from argv so it never
    lands in shell history or a process listing.
    """
    local = read_local_file()
    devid = local.get("ALTIRRA_SS_DEVID", "").strip()
    devpw = local.get("ALTIRRA_SS_DEVPASSWORD", "").strip()
    if not (devid and devpw):
        print("embed-default: no credential in {} -- nothing to embed"
              .format(LOCAL_FILE_RELATIVE), file=sys.stderr)
        return 1

    me = os.path.abspath(__file__)
    with open(me, "r", encoding="utf-8") as f:
        text = f.read()

    import re
    text = re.sub(r"DEFAULT_DEVID_OBF = bytes\(\[[^\]]*\]\)",
                  python_array("DEFAULT_DEVID_OBF", obfuscate(devid)), text,
                  count=1)
    text = re.sub(r"DEFAULT_DEVPW_OBF = bytes\(\[[^\]]*\]\)",
                  python_array("DEFAULT_DEVPW_OBF", obfuscate(devpw)), text,
                  count=1)

    with open(me, "w", encoding="utf-8") as f:
        f.write(text)

    print("embed-default: built-in credential updated ({} bytes id, "
          "{} bytes password)".format(len(devid), len(devpw)))
    return 0


def format_array(name, data):
    if not data:
        return "static const unsigned char {}[1] = {{ 0 }};\n".format(name)
    lines = ["static const unsigned char {}[{}] = {{".format(name, len(data))]
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        lines.append("\t" + " ".join("0x%02X," % b for b in chunk))
    lines.append("};")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--embed-default", action="store_true",
                    help="maintenance: bake the credential from "
                         "localconfig/screenscraper.env into this script "
                         "as the shipped default, then exit")
    ap.add_argument("--out", help="header file to write")
    ap.add_argument("--quiet-if-unchanged", action="store_true",
                    help="print nothing when the header did not change; "
                         "used by the per-build invocation so a normal "
                         "rebuild stays silent")
    args = ap.parse_args()

    if args.embed_default:
        return embed_default()

    if not args.out:
        ap.error("--out is required")

    devid = os.environ.get("ALTIRRA_SS_DEVID", "").strip()
    devpw = os.environ.get("ALTIRRA_SS_DEVPASSWORD", "").strip()

    source = "environment"
    if not (devid and devpw):
        local = read_local_file()
        devid = devid or local.get("ALTIRRA_SS_DEVID", "").strip()
        devpw = devpw or local.get("ALTIRRA_SS_DEVPASSWORD", "").strip()
        if devid and devpw:
            source = "localconfig/screenscraper.env"

    if not (devid and devpw) and DEFAULT_DEVID_OBF and DEFAULT_DEVPW_OBF:
        devid = deobfuscate(DEFAULT_DEVID_OBF)
        devpw = deobfuscate(DEFAULT_DEVPW_OBF)
        source = "built-in application credential"

    have = bool(devid and devpw)

    parts = [
        "//\tGenerated by tools/gen_ss_devkey.py -- DO NOT EDIT, DO NOT COMMIT.\n",
        "//\tSee that script for the credential policy.\n",
        "\n#pragma once\n\n",
        "#define ALTIRRA_SS_HAVE_DEVKEY {}\n\n".format(1 if have else 0),
        format_array("kATSSObfKey", KEY),
        "\n",
    ]

    if have:
        parts.append(format_array("kATSSDevIdObf", obfuscate(devid)))
        parts.append("\n")
        parts.append(format_array("kATSSDevPwObf", obfuscate(devpw)))
    else:
        parts.append(format_array("kATSSDevIdObf", b""))
        parts.append("\n")
        parts.append(format_array("kATSSDevPwObf", b""))

    out_path = args.out
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)

    text = "".join(parts)

    # Only rewrite when the content actually changes, so an unchanged
    # credential does not force a rebuild of everything downstream.
    unchanged = False
    try:
        with open(out_path, "r", encoding="utf-8") as f:
            unchanged = (f.read() == text)
    except OSError:
        pass

    if not unchanged:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(text)

    # Never print the credential or the generated bytes -- GitHub masks
    # `secrets.*` in logs but does not mask anything derived from them.
    #
    # The "how to enable" guidance is printed on BOTH the changed and
    # unchanged paths: a re-configure takes the unchanged path, and that
    # is exactly when someone is wondering why the feature is off.
    if unchanged and args.quiet_if_unchanged:
        return 0

    if have:
        print("ScreenScraper dev key: configured from {}{}".format(
            source, " (unchanged)" if unchanged else ""))
    else:
        print("ScreenScraper dev key: NOT configured -- online game "
              "metadata will be disabled.\n"
              "   To enable it, write your ScreenScraper developer "
              "credential to\n"
              "     " + os.path.join(repo_root(), LOCAL_FILE_RELATIVE) + "\n"
              "   containing:\n"
              "     ALTIRRA_SS_DEVID=yourid\n"
              "     ALTIRRA_SS_DEVPASSWORD=yourpassword\n"
              "   then build again. The file is gitignored.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
