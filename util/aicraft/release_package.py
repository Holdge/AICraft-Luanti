#!/usr/bin/env python3
# Added for AICraft on 2026-07-27; see AICRAFT_CHANGES.md.
"""Create auditable, deterministic AICraft-Luanti release packages.

The archive checksum is intentionally written outside the archive by the
release workflow.  Embedding an archive's own checksum would be circular.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import shutil
import stat
import tarfile
import zipfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


SCHEMA_VERSION = 1


def positive_epoch(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("source epoch must be positive")
    return parsed


def commit_hash(value: str) -> str:
    if len(value) != 40 or any(character not in "0123456789abcdef" for character in value):
        raise argparse.ArgumentTypeError("commit must be a 40-character lowercase SHA-1")
    return value


def parse_dependency(value: str) -> dict[str, str]:
    try:
        name, location, digest = value.split("|", 2)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "dependency must have NAME|LOCATION|SHA256 format"
        ) from error
    if not name or not location or len(digest) != 64:
        raise argparse.ArgumentTypeError(
            "dependency must have non-empty name/location and a SHA-256 digest"
        )
    if any(character not in "0123456789abcdef" for character in digest):
        raise argparse.ArgumentTypeError("dependency digest must be lowercase hexadecimal")
    return {"name": name, "location": location, "sha256": digest}


def write_manifest(args: argparse.Namespace) -> None:
    build_arguments = [
        line.rstrip("\n")
        for line in args.build_arguments.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    manifest: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "product": "AICraft-Luanti",
        "version": args.version,
        "target": args.target,
        "source": {
            "repository": args.repository,
            "commit": args.commit,
            "upstream_luanti_5_16_1_commit": args.upstream_commit,
            "source_date_epoch": args.source_epoch,
            "source_date": datetime.fromtimestamp(
                args.source_epoch, timezone.utc
            ).isoformat().replace("+00:00", "Z"),
        },
        "build": {
            "workflow": args.workflow,
            "run_id": args.run_id,
            "runner": args.runner,
            "arguments": build_arguments,
            "dependencies": args.dependency,
        },
        "code_signing": {
            "status": args.signing_status,
            "notarized": args.notarized,
        },
        "license": "LGPL-2.1-or-later",
        "modification_notice": "AICRAFT_CHANGES.md",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def entries(root: Path) -> Iterable[Path]:
    yield root
    yield from sorted(root.rglob("*"), key=lambda path: path.as_posix())


def normalized_mode(path: Path) -> int:
    mode = path.lstat().st_mode
    if path.is_symlink():
        return stat.S_IFLNK | 0o777
    if path.is_dir():
        return stat.S_IFDIR | 0o755
    executable = mode & 0o111
    return stat.S_IFREG | (0o755 if executable else 0o644)


def archive_name(root: Path, path: Path) -> str:
    if path == root:
        return root.name
    return f"{root.name}/{path.relative_to(root).as_posix()}"


def create_tar_gz(root: Path, output: Path, epoch: int) -> None:
    with output.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for path in entries(root):
                    name = archive_name(root, path)
                    info = archive.gettarinfo(str(path), arcname=name)
                    info.uid = 0
                    info.gid = 0
                    info.uname = "root"
                    info.gname = "root"
                    info.mtime = epoch
                    info.mode = normalized_mode(path) & 0o7777
                    if info.isfile():
                        with path.open("rb") as handle:
                            archive.addfile(info, handle)
                    else:
                        archive.addfile(info)


def zip_timestamp(epoch: int) -> tuple[int, int, int, int, int, int]:
    # The ZIP format cannot represent dates earlier than 1980.
    instant = datetime.fromtimestamp(max(epoch, 315532800), timezone.utc)
    return (
        instant.year,
        instant.month,
        instant.day,
        instant.hour,
        instant.minute,
        instant.second - instant.second % 2,
    )


def create_zip(root: Path, output: Path, epoch: int) -> None:
    timestamp = zip_timestamp(epoch)
    with zipfile.ZipFile(
        output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for path in entries(root):
            name = archive_name(root, path)
            if path.is_dir() and not path.is_symlink():
                name += "/"
            info = zipfile.ZipInfo(name, timestamp)
            info.create_system = 3
            info.external_attr = normalized_mode(path) << 16
            if path.is_symlink():
                payload = os.readlink(path).encode("utf-8")
            elif path.is_dir():
                payload = b""
            else:
                payload = None
            info.compress_type = zipfile.ZIP_DEFLATED
            if payload is not None:
                archive.writestr(
                    info,
                    payload,
                    compress_type=zipfile.ZIP_DEFLATED,
                    compresslevel=9,
                )
            else:
                with path.open("rb") as source, archive.open(info, "w", force_zip64=True) as target:
                    shutil.copyfileobj(source, target, length=1024 * 1024)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def create_archive(args: argparse.Namespace) -> None:
    root = args.root.resolve()
    output = args.output.resolve()
    if not root.is_dir() or root.is_symlink():
        raise SystemExit("archive root must be a real directory")
    if output == root or root in output.parents:
        raise SystemExit("archive output must be outside the archive root")
    output.parent.mkdir(parents=True, exist_ok=True)
    if args.format == "tar.gz":
        create_tar_gz(root, output, args.source_epoch)
    else:
        create_zip(root, output, args.source_epoch)

    digest = sha256_file(output)
    args.checksum.parent.mkdir(parents=True, exist_ok=True)
    args.checksum.write_text(f"{digest}  {output.name}\n", encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    manifest = subparsers.add_parser("manifest")
    manifest.add_argument("--output", type=Path, required=True)
    manifest.add_argument("--version", required=True)
    manifest.add_argument("--target", required=True)
    manifest.add_argument("--repository", required=True)
    manifest.add_argument("--commit", type=commit_hash, required=True)
    manifest.add_argument("--upstream-commit", type=commit_hash, required=True)
    manifest.add_argument("--source-epoch", type=positive_epoch, required=True)
    manifest.add_argument("--workflow", required=True)
    manifest.add_argument("--run-id", required=True)
    manifest.add_argument("--runner", required=True)
    manifest.add_argument("--build-arguments", type=Path, required=True)
    manifest.add_argument("--dependency", type=parse_dependency, action="append", default=[])
    manifest.add_argument(
        "--signing-status",
        choices=("unsigned", "ad-hoc-only", "developer-id"),
        required=True,
    )
    manifest.add_argument("--notarized", action=argparse.BooleanOptionalAction, default=False)
    manifest.set_defaults(handler=write_manifest)

    archive = subparsers.add_parser("archive")
    archive.add_argument("--root", type=Path, required=True)
    archive.add_argument("--output", type=Path, required=True)
    archive.add_argument("--checksum", type=Path, required=True)
    archive.add_argument("--format", choices=("tar.gz", "zip"), required=True)
    archive.add_argument("--source-epoch", type=positive_epoch, required=True)
    archive.set_defaults(handler=create_archive)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    args.handler(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
