#!/usr/bin/env bash
# OmaZettel installer, for Omarchy and Arch Linux.
#
#   curl -fsSL https://raw.githubusercontent.com/mcsmith3/omazettel/master/install.sh | bash
#
# Installs the pacman package from the latest release, after checking it
# against the release's SHA256SUMS. Where there is no package for this machine
# (such as aarch64), it builds one from the release's source instead. Run it
# again to update; remove it with: sudo pacman -R omazettel
#
# Environment:
#   OMAZETTEL_REF          the release tag to install, such as omazettel-v0.8.0 (default: the latest)
#   OMAZETTEL_FROM_SOURCE  set to 1 to build from source even where there is a package

set -euo pipefail

REPO="mcsmith3/omazettel"
REF="${OMAZETTEL_REF:-}"
FROM_SOURCE="${OMAZETTEL_FROM_SOURCE:-}"

info() { printf '  %s\n' "$*"; }
die() { printf '\e[31mError:\e[0m %s\n' "$*" >&2; exit 1; }

command -v pacman >/dev/null \
  || die "this installs a pacman package, and there is no pacman here (OmaZettel is for Omarchy and Arch Linux).
  To build it by hand, see https://github.com/$REPO#building-from-source"
command -v curl >/dev/null || die "curl is needed"
[[ $EUID -ne 0 ]] || die "run this as your own user, not root; it asks for sudo when it installs"

# Piped from curl, this script is on standard input: pacman and makepkg ask
# their questions on the terminal.
TTY=/dev/null
CONFIRM=(--noconfirm)
if [[ -r /dev/tty ]] && { : </dev/tty; } 2>/dev/null; then
  TTY=/dev/tty
  CONFIRM=()
fi

# Installing needs sudo. Ask for the password up front, so a wrong password or
# an account without sudo gets its own message instead of looking like a
# pacman problem.
# With "terminal-only", it only checks there is somewhere to type the
# password: makepkg runs sudo -k, which asks again anyway.
get_sudo() {
  sudo -n true 2>/dev/null && return 0
  if [[ $TTY != /dev/tty ]]; then
    die "installing needs your sudo password, and there is no terminal to ask for it in.
  Run the install line in a terminal. Nothing was installed."
  fi
  [[ ${1:-} == terminal-only ]] && return 0
  info "Installing needs your password (for sudo)"
  sudo -v <"$TTY" || die "sudo did not accept the password, or this account is not allowed to use sudo.
  Nothing was installed. Run the line again and enter your login password."
}

# Says why pacman (or makepkg) stopped, from the errors it printed to $1.
explain_failure() {
  local log="$1" what="$2"
  if grep -qiE "incorrect password|not in the sudoers|not allowed to run sudo|a password is required" "$log"; then
    die "sudo did not accept the password, or this account is not allowed to use sudo.
  Nothing was installed. Run the line again and enter your login password."
  elif grep -qiE "could not satisfy dependencies|unable to satisfy dependency|requires qt6|target not found: qt6" "$log"; then
    die "pacman could not install a library OmaZettel needs (usually a newer Qt 6).
  Update the system with: sudo pacman -Syu
  then run the install line again. Nothing was installed."
  elif grep -qi "unable to lock database" "$log"; then
    die "another package manager is running (pacman's database is locked).
  Wait for it to finish (or close it) and run the install line again.
  If nothing else is running, a crash may have left the lock: sudo rm /var/lib/pacman/db.lck"
  elif grep -qiE "exists in filesystem|conflicting files" "$log"; then
    die "files OmaZettel installs are already on this system, from another package (pacman lists them above).
  Find their owner with: pacman -Qo <file>, remove that package, and run the install line again. Nothing was installed."
  elif [[ $what == build ]] && grep -qE "==> ERROR: A failure occurred in (build|package)" "$log"; then
    die "building OmaZettel failed (see the compiler's messages above). Nothing was installed.
  Please report it, with that output, at https://github.com/$REPO/issues"
  elif ! grep -qi "error" "$log"; then
    die "pacman stopped without installing it (if you answered no, nothing changed)."
  else
    die "pacman did not install it; its message is above. Nothing was installed."
  fi
}

ARCH="$(uname -m)"
PACKAGE="omazettel-$ARCH.pkg.tar.zst"
if [[ -n $REF ]]; then
  BASE="https://github.com/$REPO/releases/download/$REF"
else
  BASE="https://github.com/$REPO/releases/latest/download"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

build_from_source() {
  local tag="$REF"
  if [[ -z $tag ]]; then
    # The latest release's tag, from where /releases/latest redirects to.
    tag="$(curl -fsSLI -o /dev/null -w '%{url_effective}' "https://github.com/$REPO/releases/latest")"
    tag="${tag##*/}"
    [[ $tag == omazettel-v* ]] || die "could not find the latest release of $REPO"
  fi
  info "Building $tag from source (this takes a few minutes)"
  command -v git >/dev/null || die "building needs git: sudo pacman -S git base-devel"
  command -v makepkg >/dev/null || die "building needs makepkg: sudo pacman -S base-devel"
  git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" "https://github.com/$REPO.git" "$TMP/omazettel" \
    || die "could not download the source of $tag"
  get_sudo terminal-only
  # makepkg -s installs Qt and the compiler first if they are missing. Its
  # errors go to the terminal as usual and to a log that explains them.
  (cd "$TMP/omazettel/pkgbuild" && makepkg -sfi "${CONFIRM[@]}" <"$TTY" 2> >(tee "$TMP/errors.log" >&2)) \
    || { sleep 0.2; explain_failure "$TMP/errors.log" build; }
}

printf '\e[1m%s\e[0m\n' "Installing OmaZettel (${REF:-latest release}, $ARCH)"

if [[ -n $FROM_SOURCE ]]; then
  build_from_source
elif curl -fsSL -o "$TMP/$PACKAGE" "$BASE/$PACKAGE" 2>/dev/null; then
  info "Fetched $PACKAGE"
  curl -fsSL -o "$TMP/SHA256SUMS" "$BASE/SHA256SUMS" \
    || die "could not fetch $BASE/SHA256SUMS; nothing was installed"
  info "Checking it"
  (cd "$TMP" && grep " $PACKAGE\$" SHA256SUMS | sha256sum --check --quiet --strict) \
    || die "$PACKAGE is not what SHA256SUMS says it should be; nothing was installed"
  get_sudo
  info "Installing it with pacman"
  sudo pacman -U "${CONFIRM[@]}" "$TMP/$PACKAGE" <"$TTY" 2> >(tee "$TMP/errors.log" >&2) \
    || { sleep 0.2; explain_failure "$TMP/errors.log" install; }
else
  info "There is no ready-made package for $ARCH in this release"
  build_from_source
fi

info "Done: $(pacman -Q omazettel)"
info "Open it from the launcher, or run: omazettel"
info "Update: run the same line again. Uninstall: sudo pacman -R omazettel (your notes stay)"
