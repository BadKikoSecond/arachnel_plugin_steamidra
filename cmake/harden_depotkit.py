#!/usr/bin/env python3
"""Make the linked depotkit safe to call from steamidra on Windows.

depotkit is compiled into steamidra_plugin.dll. Two bugs in that code become
an invalid write inside the plugin image when an owns-download decrypts
manifests (Valheim with a selected Steam DLC walks every lua depot):

* BCryptSetProperty writes through the chaining-mode buffer. The call sites
  passed a string literal, which lives in the plugin .rdata.
* Manifest section lengths were applied with pointer arithmetic before checking
  that the section fits in the file.
"""

from __future__ import annotations

import sys
from pathlib import Path

MARKER = "steamidra: writable bcrypt chain mode"


def patch_crypto(text: str) -> str:
    old = """    const wchar_t *chain = useCbc ? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB;
    if (BCryptSetProperty(slot, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(chain)),
                          static_cast<ULONG>((wcslen(chain) + 1) * sizeof(wchar_t)), 0)"""
    new = f"""    // {MARKER}
    wchar_t chainBuf[32] = {{}};
    const wchar_t *chainLit = useCbc ? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB;
    wcsncpy(chainBuf, chainLit, 31);
    chainBuf[31] = L'\\0';
    if (BCryptSetProperty(slot, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(chainBuf),
                          static_cast<ULONG>((wcslen(chainBuf) + 1) * sizeof(wchar_t)), 0)"""
    if old not in text:
        if MARKER in text:
            return text
        raise SystemExit("depot_crypto.cpp: BCryptSetProperty pattern missing")
    return text.replace(old, new, 1)


HELPER = f"""
// {MARKER}
// BCryptSetProperty writes into pbInput. A string literal is in the plugin
// image (.rdata) because this file is linked into steamidra_plugin.dll.
inline void setBcryptChainMode(BCRYPT_ALG_HANDLE alg, const wchar_t *literal)
{{
    wchar_t mode[32] = {{}};
    if (!alg || !literal)
        return;
    wcsncpy(mode, literal, 31);
    mode[31] = L'\\0';
    const ULONG bytes = static_cast<ULONG>((wcslen(mode) + 1) * sizeof(wchar_t));
    BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(mode), bytes, 0);
}}

"""


def patch_session(text: str) -> str:
    text2 = text
    for mode in ("BCRYPT_CHAIN_MODE_ECB", "BCRYPT_CHAIN_MODE_CBC"):
        old = (
            "    BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, "
            f"reinterpret_cast<PUCHAR>(const_cast<wchar_t *>({mode})),\n"
            f"                      sizeof({mode}), 0);"
        )
        new = f"    setBcryptChainMode(alg, {mode});"
        if old not in text2:
            if "setBcryptChainMode" in text2:
                continue
            raise SystemExit(f"session.cpp: missing {mode} BCryptSetProperty")
        text2 = text2.replace(old, new)
    if "setBcryptChainMode" in text and HELPER.strip() in text:
        return text2
    anchor = "WinsockInit gWinsock;\n"
    if "inline void setBcryptChainMode" not in text2:
        if anchor not in text2:
            raise SystemExit("session.cpp: WinsockInit anchor missing")
        text2 = text2.replace(anchor, anchor + HELPER, 1)
    return text2


MANIFEST_OLD = """        if (magic == kProtoPayloadMagic) {
            const uint32_t len = c.u32();
            Cursor sub{c.p, c.p + len};
            c.p += len;
            parsePayload(m, sub);
        } else if (magic == kProtoMetadataMagic) {
            const uint32_t len = c.u32();
            Cursor sub{c.p, c.p + len};
            c.p += len;
            parseMetadata(m, sub);
        } else if (magic == kProtoSignatureMagic) {
            const uint32_t len = c.u32();
            c.skip(len);
        } else {"""

MANIFEST_NEW = """        if (magic == kProtoPayloadMagic || magic == kProtoMetadataMagic
            || magic == kProtoSignatureMagic) {
            const uint32_t len = c.u32();
            // Reject a length that runs past the file before any pointer add.
            // A wrapped length used to build a cursor into unrelated mapped
            // pages (including this DLL) and the decrypt path then wrote there.
            if (static_cast<size_t>(len) > c.left())
                throw std::runtime_error("manifest section truncated");
            if (magic == kProtoSignatureMagic) {
                c.skip(len);
            } else {
                Cursor sub{c.p, c.p + len};
                c.p += len;
                if (magic == kProtoPayloadMagic)
                    parsePayload(m, sub);
                else
                    parseMetadata(m, sub);
            }
        } else {"""


def patch_manifest(text: str) -> str:
    if "manifest section truncated" in text:
        return text
    if MANIFEST_OLD not in text:
        raise SystemExit("manifest.cpp: section parser pattern missing")
    return text.replace(MANIFEST_OLD, MANIFEST_NEW, 1)


def write_if_changed(path: Path, text: str) -> None:
    if path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: harden_depotkit.py DEPOTKIT_SRC")
    root = Path(sys.argv[1])
    crypto = root / "src" / "crypto" / "depot_crypto.cpp"
    session = root / "src" / "steam" / "session.cpp"
    manifest = root / "src" / "manifest" / "manifest.cpp"
    for p in (crypto, session, manifest):
        if not p.is_file():
            raise SystemExit(f"missing {p}")
    write_if_changed(crypto, patch_crypto(crypto.read_text(encoding="utf-8")))
    write_if_changed(session, patch_session(session.read_text(encoding="utf-8")))
    write_if_changed(manifest, patch_manifest(manifest.read_text(encoding="utf-8")))


if __name__ == "__main__":
    main()
