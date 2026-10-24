#!/bin/bash
# LighthouseSync for the Steam Frame: installs or updates lhsyncd and the questlhsync_frame driver from the latest
# release, and keeps them up to date. Run it on the headset, in desktop mode's terminal or over ssh:
#
#   curl -fsSL https://github.com/ronaldburns/LighthouseSync/releases/latest/download/install-frame.sh | bash
#
# Options (after "bash -s --" when piped): --no-auto-update, --version <tag>, --uninstall
#
# It downloads the release's Frame package, checks it against the release's SHA256SUMS and runs the package's own
# install.sh, which restarts SteamVR on the headset to load the driver. Installs where QuestLHSync's package does, so it
# updates an existing QuestLHSync in place.
#
# Updates after that (unless --no-auto-update): a systemd user timer looks for a new release every 6 hours and
# downloads it, checked the same way, and a service installs it the next time the headset starts, before its SteamVR
# does, so nothing restarts while the headset is in use.
set -euo pipefail

REPO=${LHS_REPO:-@REPO@}   # the release workflow writes the repository it was built from
case $REPO in @*) REPO=ronaldburns/LighthouseSync ;; esac
DEST=$HOME/.local/share/questlhsync
LHS=$DEST/lhs   # the version installed, an update waiting for the next start, this script
UNITS=$HOME/.config/systemd/user
VRPATHREG=/opt/steamvr/bin/linuxarm64/vrpathreg
# desktop mode's terminal has XDG_RUNTIME_DIR=/run/user/<uid>/nested_plasma, where systemctl --user finds no systemd
export XDG_RUNTIME_DIR=/run/user/$(id -u)

TMP=""   # a download folder to remove on exit
trap 'rm -rf "$TMP"' EXIT

say() { printf '%s\n' "$*"; }
fail() { printf 'LighthouseSync: %s\n' "$*" >&2; exit 1; }

latest_tag() {  # GitHub answers releases/latest with a redirect to releases/tag/<tag>
  local url
  url=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "https://github.com/$REPO/releases/latest") || return 1
  case $url in */releases/tag/?*) printf '%s\n' "${url##*/}" ;; *) return 1 ;; esac
}

# get <tag> <file> <dir>: downloads one file of the release into dir and checks it against the release's SHA256SUMS
get() {
  local base="https://github.com/$REPO/releases/download/$1"
  [ -f "$3/SHA256SUMS" ] || curl -fsSL -o "$3/SHA256SUMS" "$base/SHA256SUMS" || fail "couldn't download $1's SHA256SUMS"
  curl -fsSL -o "$3/$2" "$base/$2" || fail "couldn't download $2 from $1"
  (cd "$3" && awk -v n="$2" '$2 == n || $2 == "*" n { print $1 "  " n }' SHA256SUMS | sha256sum -c --status 2>/dev/null) ||
    fail "$2 doesn't match the SHA-256 in $1's SHA256SUMS"
}

# fetch <tag> <dir>: the release's Frame package, checked and unpacked into dir/QuestLHSync-frame
fetch() {
  local name="LighthouseSync-frame-${1#v}.tar.gz"
  get "$1" "$name" "$2"
  tar -xzf "$2/$name" -C "$2"
  [ -f "$2/QuestLHSync-frame/install.sh" ] || fail "$name has no QuestLHSync-frame/install.sh"
}

installed() { cat "$LHS/version" 2>/dev/null || true; }

install_package() {  # <unpacked dir> <tag>
  sh "$1/QuestLHSync-frame/install.sh"
  mkdir -p "$LHS"
  cp "$1/QuestLHSync-frame/uninstall.sh" "$LHS/uninstall.sh"
  printf '%s\n' "$2" > "$LHS/version"
}

write_units() {
  mkdir -p "$UNITS"
  cat > "$UNITS/lighthousesync-update.service" <<EOF
[Unit]
Description=LighthouseSync: download a new release for the next start

[Service]
Type=oneshot
Environment=LHS_REPO=$REPO
ExecStart=%h/.local/share/questlhsync/lhs/install-frame.sh --stage
EOF
  cat > "$UNITS/lighthousesync-update.timer" <<'EOF'
[Unit]
Description=LighthouseSync: look for a new release every 6 hours

[Timer]
OnBootSec=5min
OnUnitActiveSec=6h
RandomizedDelaySec=10min

[Install]
WantedBy=timers.target
EOF
  # before the headset's SteamVR loads the driver, so the package's install.sh has nothing to restart
  cat > "$UNITS/lighthousesync-apply.service" <<'EOF'
[Unit]
Description=LighthouseSync: install a downloaded release before SteamVR starts
ConditionPathExists=%h/.local/share/questlhsync/lhs/staged/QuestLHSync-frame/install.sh
Before=steamvr.service

[Service]
Type=oneshot
ExecStart=%h/.local/share/questlhsync/lhs/install-frame.sh --apply

[Install]
WantedBy=default.target
EOF
}

remove_units() {
  systemctl --user disable --now lighthousesync-update.timer lighthousesync-apply.service >/dev/null 2>&1 || true
  rm -f "$UNITS"/lighthousesync-update.service "$UNITS"/lighthousesync-update.timer "$UNITS"/lighthousesync-apply.service
  systemctl --user daemon-reload
}

do_install() {
  local tag=$1 auto=$2
  [ -x "$VRPATHREG" ] || fail "SteamVR not found in /opt/steamvr: run this on a Steam Frame"
  [ -n "$tag" ] || tag=$(latest_tag) || fail "couldn't find $REPO's latest release"
  TMP=$(mktemp -d)
  say "Downloading LighthouseSync $tag for the Steam Frame from $REPO"
  fetch "$tag" "$TMP"
  say "SHA-256 matches"
  install_package "$TMP" "$tag"
  rm -rf "$LHS/staged"
  if [ "$auto" = 1 ]; then
    get "$tag" install-frame.sh "$TMP"
    install -m 755 "$TMP/install-frame.sh" "$LHS/install-frame.sh"
    write_units
    systemctl --user daemon-reload
    systemctl --user enable --now lighthousesync-update.timer >/dev/null
    systemctl --user enable lighthousesync-apply.service >/dev/null 2>&1
    say "Automatic updates are on: new releases download in the background and install when the headset restarts."
  else
    remove_units
    rm -f "$LHS/install-frame.sh"
    say "Automatic updates are off. Run this again to update."
  fi
  say "LighthouseSync $tag is installed on the headset."
}

# timer: download a newer release into $LHS/staged for the next start
do_stage() {
  local tag
  tag=$(latest_tag) || fail "couldn't find $REPO's latest release"
  [ "$tag" = "$(installed)" ] && { say "$tag is installed"; return; }
  [ "$tag" = "$(cat "$LHS/staged/version" 2>/dev/null || true)" ] && { say "$tag is waiting for the next start"; return; }
  TMP=$(mktemp -d "$LHS/staging.XXXXXX")
  fetch "$tag" "$TMP"
  get "$tag" install-frame.sh "$TMP"
  printf '%s\n' "$tag" > "$TMP/version"
  rm -rf "$LHS/staged"
  mv "$TMP" "$LHS/staged"
  TMP=""
  say "Downloaded $tag: it installs when the headset restarts"
}

# at login, before SteamVR: install what do_stage downloaded
do_apply() {
  local staged=$LHS/staged tag
  [ -f "$staged/QuestLHSync-frame/install.sh" ] || return 0
  tag=$(cat "$staged/version")
  install_package "$staged" "$tag"
  install -m 755 "$staged/install-frame.sh" "$LHS/install-frame.sh"  # the updater from the same release
  rm -rf "$staged"
  say "Installed $tag"
}

do_uninstall() {
  remove_units
  if [ -f "$LHS/uninstall.sh" ]; then
    sh "$LHS/uninstall.sh"
  else  # what the package's uninstall.sh does
    systemctl --user disable --now questlhsync.service 2>/dev/null || true
    rm -f "$UNITS/questlhsync.service"
    systemctl --user daemon-reload
    "$VRPATHREG" removedriver "$DEST/questlhsync_frame" >/dev/null 2>&1 || true
    rm -rf "$DEST"
  fi
  say "LighthouseSync removed from the headset (restart SteamVR on the headset to unload its driver)."
}

mode=install tag="" auto=1
while [ $# -gt 0 ]; do
  case $1 in
    --no-auto-update) auto=0 ;;
    --version) tag=${2:?--version needs a tag}; shift ;;
    --uninstall) mode=uninstall ;;
    --stage) mode=stage ;;
    --apply) mode=apply ;;
    *) fail "unknown option $1" ;;
  esac
  shift
done
case $mode in
  install) do_install "$tag" "$auto" ;;
  stage) do_stage ;;
  apply) do_apply ;;
  uninstall) do_uninstall ;;
esac
