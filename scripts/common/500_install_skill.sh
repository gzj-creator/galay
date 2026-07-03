#!/usr/bin/env bash
set -euo pipefail

usage()
{
  echo "usage: $0 <source_skill_dir> <install_dir>" >&2
}

if [[ "$#" -ne 2 ]]; then
  usage
  exit 2
fi

source_dir=$1
install_dir=$2

if [[ ! -d "${source_dir}" ]]; then
  echo "source skill dir not found: ${source_dir}" >&2
  exit 1
fi

if [[ ! -f "${source_dir}/SKILL.md" ]]; then
  echo "source skill dir missing SKILL.md: ${source_dir}" >&2
  exit 1
fi

skill_name=$(basename "${source_dir}")
if [[ -z "${skill_name}" || "${skill_name}" == "." || "${skill_name}" == "/" ]]; then
  echo "invalid source skill dir: ${source_dir}" >&2
  exit 1
fi

mkdir -p "${install_dir}"

source_abs=$(cd "${source_dir}" && pwd -P)
install_abs=$(cd "${install_dir}" && pwd -P)
target_abs="${install_abs}/${skill_name}"

if [[ "${source_abs}" == "${target_abs}" ]]; then
  echo "source and target skill dirs are the same: ${source_abs}" >&2
  exit 1
fi

rm -rf "${target_abs}"
cp -R "${source_abs}" "${target_abs}"

echo "${target_abs}"
