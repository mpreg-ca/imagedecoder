#!/usr/bin/env python3
"""Regenerates imagedecoder/cmyk.h from an ICC profile.

    tools/make-cmyk-header.py USWebCoatedSWOP.icc
"""
import sys
import zlib


def main(path: str) -> None:
    data = open(path, "rb").read()
    comp = zlib.compress(data, 9)

    out = [
        "#pragma once",
        "",
        "/* US Web Coated (SWOP) v2 CMYK profile, deflated: fallback for a CMYK/YCCK",
        " * JPEG with no profile of its own. Raw is %d bytes; this is %d." % (len(data), len(comp)),
        " *",
        " * const so it lands in .rodata, not .data - avoids a dirty private page in",
        " * every process that loads the library, decoded or not.",
        " *",
        " * Regenerate with tools/make-cmyk-header.py. */",
        "",
        "static const unsigned char CMYK_USWebCoatedSWOP_icc_z[] = {",
    ]
    for i in range(0, len(comp), 12):
        out.append("  " + " ".join("0x%02x," % b for b in comp[i:i + 12]))
    out += [
        "};",
        "",
        "static const unsigned int CMYK_USWebCoatedSWOP_icc_z_len = %d;" % len(comp),
        "static const unsigned int CMYK_USWebCoatedSWOP_icc_len = %d;" % len(data),
    ]
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
